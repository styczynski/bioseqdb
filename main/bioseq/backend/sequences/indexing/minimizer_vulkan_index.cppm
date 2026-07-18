/**
 * @file minimizer_vulkan_index.cppm
 * @brief DOC0307 Phase B — GPU-resident view over `MinimizerHashmapFragmentIndex`
 *        and the batch/wave-scoped upload cache.
 *
 * Reuses the existing serialized bucket ABI (`SerializedBucket`) as the GPU slot
 * record; derives either an open-addressed or hash-sorted dense control array
 * plus the positions array at upload time. Keeps
 * `minimizer_hashmap_index.cppm` storage-free — all Vulkan/cache
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
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan.h>
#include <fmt/format.h>

export module bioseq.indexing.minimizer_vulkan;

import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.common.radix_sort;
import bioseq.common.mlogging;

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

struct GpuIndexDirectFillInvariant final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "GPU minimizer index direct-fill invariant failed";
    }
};

/// Typed diagnostic for the legacy CPU-built minimizer-index upload.  Probe
/// callers may continue to use the nullable handle, while strict pipeline
/// callers pass an output record and preserve the exact predicate that made
/// the handle unavailable.  This record is evidence only; it does not permit a
/// CPU fallback.
enum class GpuIndexUploadDeclineReason : std::uint8_t {
    none,
    device_abi_overflow,
    storage_buffer_range_exceeded,
    direct_fill_invariant_failed,
    resident_memory_admission_denied,
    upload_peak_memory_admission_denied,
    non_host_visible_device_memory,
};

enum class GpuIndexRepresentation : std::uint8_t {
    open_addressed_70,
    sorted_dense,
};

[[nodiscard]] inline constexpr std::string_view gpu_index_representation_name(
        GpuIndexRepresentation representation) noexcept {
    switch (representation) {
        case GpuIndexRepresentation::open_addressed_70:
            return "open_addressed_70";
        case GpuIndexRepresentation::sorted_dense:
            return "sorted_dense";
    }
    return "unknown";
}

struct GpuIndexUploadDecline {
    GpuIndexUploadDeclineReason reason = GpuIndexUploadDeclineReason::none;
    std::uint64_t resident_bytes = 0;
    std::uint64_t upload_peak_bytes = 0;
    std::uint64_t slot_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t unique_key_count = 0;
    std::uint64_t slot_capacity = 0;
    std::uint64_t staging_bytes = 0;
    std::uint64_t actual_allocation_bytes = 0;
    std::uint64_t fully_touched_bytes = 0;
    std::uint64_t reflected_footprint_delta_bytes = 0;
    std::uint64_t max_storage_buffer_range = 0;
    std::uint32_t load_ppm = 0;
    std::uint32_t max_probe = 0;
    bool materialization_permit_released = false;
    bool process_footprint_reflected = false;
    GpuIndexRepresentation representation =
        GpuIndexRepresentation::open_addressed_70;
    bioseq::parallelism::MemoryAdmissionFailure admission{};

    [[nodiscard]] constexpr bool declined() const noexcept {
        return reason != GpuIndexUploadDeclineReason::none;
    }
};

[[nodiscard]] inline constexpr std::string_view
gpu_index_upload_decline_reason_name(
    GpuIndexUploadDeclineReason reason) noexcept
{
    switch (reason) {
        case GpuIndexUploadDeclineReason::none:
            return "none";
        case GpuIndexUploadDeclineReason::device_abi_overflow:
            return "device_abi_overflow";
        case GpuIndexUploadDeclineReason::storage_buffer_range_exceeded:
            return "storage_buffer_range_exceeded";
        case GpuIndexUploadDeclineReason::direct_fill_invariant_failed:
            return "direct_fill_invariant_failed";
        case GpuIndexUploadDeclineReason::resident_memory_admission_denied:
            return "resident_memory_admission_denied";
        case GpuIndexUploadDeclineReason::upload_peak_memory_admission_denied:
            return "upload_peak_memory_admission_denied";
        case GpuIndexUploadDeclineReason::non_host_visible_device_memory:
            return "non_host_visible_device_memory";
    }
    return "unknown";
}

[[nodiscard]] inline std::string format_gpu_index_upload_decline(
    const GpuIndexUploadDecline& decline)
{
    const std::string_view reason =
        gpu_index_upload_decline_reason_name(decline.reason);
    if (decline.reason
            == GpuIndexUploadDeclineReason::resident_memory_admission_denied
        || decline.reason
            == GpuIndexUploadDeclineReason::upload_peak_memory_admission_denied) {
        const auto& admission = decline.admission;
        return fmt::format(
            "reason={} admission_reason={} required_bytes={} "
            "allocatable_after_reservations_bytes={} reserved_bytes={} "
            "budget_ceiling_bytes={} process_footprint_bytes={} "
            "system_available_bytes={} system_total_bytes={} "
            "process_soft_cap_bytes={} resident_bytes={} upload_peak_bytes={} "
            "slot_bytes={} position_bytes={} unique_key_count={} "
            "slot_capacity={} load_ppm={} max_probe={} representation={} "
            "staging_bytes={} actual_allocation_bytes={} "
            "fully_touched_bytes={} reflected_footprint_delta_bytes={} "
            "materialization_permit_released={} "
            "process_footprint_reflected={} max_storage_buffer_range={}",
            reason,
            bioseq::parallelism::memory_admission_failure_reason_name(
                admission.reason),
            admission.requested_bytes,
            admission.allocatable_after_reservations_bytes,
            admission.reserved_bytes,
            admission.budget_ceiling_bytes,
            admission.process_footprint_bytes,
            admission.system_available_bytes,
            admission.system_total_bytes,
            admission.process_soft_cap_bytes,
            decline.resident_bytes,
            decline.upload_peak_bytes,
            decline.slot_bytes,
            decline.position_bytes,
            decline.unique_key_count,
            decline.slot_capacity,
            decline.load_ppm,
            decline.max_probe,
            gpu_index_representation_name(decline.representation),
            decline.staging_bytes,
            decline.actual_allocation_bytes,
            decline.fully_touched_bytes,
            decline.reflected_footprint_delta_bytes,
            decline.materialization_permit_released,
            decline.process_footprint_reflected,
            decline.max_storage_buffer_range);
    }
    return fmt::format(
        "reason={} resident_bytes={} upload_peak_bytes={} slot_bytes={} "
        "position_bytes={} unique_key_count={} slot_capacity={} load_ppm={} "
        "max_probe={} representation={} staging_bytes={} "
        "actual_allocation_bytes={} fully_touched_bytes={} "
        "reflected_footprint_delta_bytes={} "
        "materialization_permit_released={} "
        "process_footprint_reflected={} max_storage_buffer_range={}",
        reason,
        decline.resident_bytes,
        decline.upload_peak_bytes,
        decline.slot_bytes,
        decline.position_bytes,
        decline.unique_key_count,
        decline.slot_capacity,
        decline.load_ppm,
        decline.max_probe,
        gpu_index_representation_name(decline.representation),
        decline.staging_bytes,
        decline.actual_allocation_bytes,
        decline.fully_touched_bytes,
        decline.reflected_footprint_delta_bytes,
        decline.materialization_permit_released,
        decline.process_footprint_reflected,
        decline.max_storage_buffer_range);
}

// Legacy raw-target/recompute cache key. It is intentionally not accepted by
// any DOC0371 persisted API below; the later atomic production-routing change
// will migrate/delete this pointer-shaped path together with its pipe callers.
struct GpuMinimizerIndexCacheKey {
    std::uintptr_t source_id = 0;
    std::uint64_t caller_identity = 0;
    std::uint64_t target_len = 0;
    std::uint64_t table_size = 0;
    std::uint64_t position_count = 0;
    bool gate_enabled = false;
    std::uint32_t gate_word_count = 0;
    std::uint32_t gate_mask = 0;
    std::uint64_t gate_digest_lo = 0;
    std::uint64_t gate_digest_hi = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> gate_canonical_words{};

    [[nodiscard]] bool operator==(
        const GpuMinimizerIndexCacheKey& other) const
    {
        if (source_id != other.source_id
            || caller_identity != other.caller_identity
            || target_len != other.target_len || table_size != other.table_size
            || position_count != other.position_count
            || gate_enabled != other.gate_enabled
            || gate_word_count != other.gate_word_count
            || gate_mask != other.gate_mask
            || gate_digest_lo != other.gate_digest_lo
            || gate_digest_hi != other.gate_digest_hi) {
            return false;
        }
        if (!gate_enabled) {
            return !gate_canonical_words && !other.gate_canonical_words;
        }
        if (!gate_canonical_words || !other.gate_canonical_words) return false;
        if (gate_canonical_words.get()
            == other.gate_canonical_words.get()) return true;
        return *gate_canonical_words == *other.gate_canonical_words;
    }
};

[[nodiscard]] inline bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity
gpu_index_gate_identity_from_key(
    const GpuMinimizerIndexCacheKey& key)
{
    return bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity{
        .enabled = key.gate_enabled,
        .word_count = key.gate_word_count,
        .mask = key.gate_mask,
        .digest_lo = key.gate_digest_lo,
        .digest_hi = key.gate_digest_hi,
        .canonical_words = key.gate_canonical_words,
    };
}

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
        h ^= splitmix64(static_cast<std::uint64_t>(k.gate_enabled) + (h << 1));
        h ^= splitmix64(k.gate_word_count + (h << 1));
        h ^= splitmix64(k.gate_mask + (h << 1));
        h ^= splitmix64(k.gate_digest_lo + (h << 1));
        h ^= splitmix64(k.gate_digest_hi + (h << 1));
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
    bool gate_enabled = false;
    std::uint32_t gate_word_count = 0;
    std::uint32_t gate_mask = 0;
    std::uint64_t gate_digest_lo = 0;
    std::uint64_t gate_digest_hi = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> gate_canonical_words{};
};

[[nodiscard]] inline GpuMinimizerIndexCacheKey make_gpu_index_cache_key(
    const MinimizerHashmapFragmentIndex& idx,
    GpuIndexCacheIdentity identity) noexcept
{
    const auto bind_gate = [identity](GpuMinimizerIndexCacheKey key) noexcept {
        key.gate_enabled = identity.gate_enabled;
        key.gate_word_count = identity.gate_word_count;
        key.gate_mask = identity.gate_mask;
        key.gate_digest_lo = identity.gate_digest_lo;
        key.gate_digest_hi = identity.gate_digest_hi;
        key.gate_canonical_words = identity.gate_canonical_words;
        return key;
    };
    if (identity.assembly_identity == 0) {
        return bind_gate(make_gpu_index_cache_key_for_in_memory(idx));
    }
    if (identity.table_size != 0 || identity.position_count != 0) {
        return bind_gate(GpuMinimizerIndexCacheKey{
            .source_id = 0u,
            .caller_identity = identity.assembly_identity,
            .target_len = static_cast<std::uint64_t>(idx.target_len),
            .table_size = identity.table_size,
            .position_count = identity.position_count,
        });
    }
    return bind_gate(
        make_gpu_index_cache_key_for_assembly(idx, identity.assembly_identity));
}

struct GpuResidentMinimizerIndex {
    // Declared first so it is destroyed last, after both resident buffers.
    bioseq::parallelism::MemoryPermit permit;
    bioseq::gpu::vulkan::Buffer slots;
    bioseq::gpu::vulkan::Buffer positions;
    bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity gate_identity{};
    GpuMinimizerIndexCacheKey key{};
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    // DOC0310 §12 — when nonzero, `slots` is a hash-sorted dense bucket array
    // that the seed lookup binary-searches; `slot_mask` is then 0. When zero,
    // `slots` is the open-addressed table and `slot_mask` is the power-of-two
    // mask. The explicit `representation` field is the host-side attestation.
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
    std::uint64_t unique_key_count = 0;
    std::uint64_t slot_capacity = 0;
    std::uint64_t staging_bytes = 0;
    std::uint32_t load_ppm = 0;
    std::uint32_t max_probe = 0;
    GpuIndexRepresentation representation =
        GpuIndexRepresentation::open_addressed_70;
    bioseq::parallelism::MemoryMaterializationCommit materialization{};
    bioseq::gpu::vulkan::GpuBuiltIndexOwnershipProof ownership{};
};
static_assert(std::is_standard_layout_v<GpuResidentMinimizerIndex>);
static_assert(offsetof(GpuResidentMinimizerIndex, permit)
    < offsetof(GpuResidentMinimizerIndex, slots));
static_assert(offsetof(GpuResidentMinimizerIndex, permit)
    < offsetof(GpuResidentMinimizerIndex, positions));

[[nodiscard]] inline std::uint32_t gpu_slot_capacity(std::uint64_t distinct) {
    // Power-of-two open addressing at <= 70% load.  ceil(10*D/7) is
    // deliberately evaluated only after an overflow guard; P=93,308,896
    // therefore plans 133,298,423 wanted slots -> C=2^27.
    if (distinct > ((std::numeric_limits<std::uint64_t>::max)() - 6u) / 10u) {
        throw GpuIndexTooLarge{};
    }
    const std::uint64_t wanted = (distinct * 10u + 6u) / 7u;
    if (wanted > (std::uint64_t{1} << 31)) {
        throw GpuIndexTooLarge{};
    }
    return static_cast<std::uint32_t>(std::bit_ceil(std::max<std::uint64_t>(2, wanted)));
}

[[nodiscard]] inline std::uint32_t gpu_index_load_ppm(
        std::uint64_t distinct,
        std::uint64_t capacity) noexcept {
    if (capacity == 0u) return 0u;
    const std::uint64_t scaled =
        bioseq::parallelism::saturating_mul(distinct, 1'000'000u);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        scaled / capacity, 1'000'000u));
}

[[nodiscard]] inline bool gpu_index_storage_ranges_fit(
        std::uint64_t slot_buffer_bytes,
        std::uint64_t position_buffer_bytes,
        std::uint64_t max_storage_buffer_range) noexcept {
    return max_storage_buffer_range != 0u
        && slot_buffer_bytes <= max_storage_buffer_range
        && position_buffer_bytes <= max_storage_buffer_range;
}

struct GpuIndexDirectFillStats {
    std::uint64_t unique_key_count = 0;
    std::uint64_t slot_capacity = 0;
    std::uint32_t load_ppm = 0;
    std::uint32_t max_probe = 0;
};

struct GpuIndexMaterializationPlan {
    GpuIndexRepresentation representation =
        GpuIndexRepresentation::open_addressed_70;
    std::uint64_t unique_key_count = 0;
    std::uint64_t slot_capacity = 0;
    std::uint64_t slot_bytes = 0;
    std::uint64_t slot_buffer_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_buffer_bytes = 0;
    std::uint64_t resident_bytes = 0;
    std::uint32_t load_ppm = 0;
};

/// Exact logical/allocation request for either seed-shader representation.
/// `sorted_dense` is deliberately unavailable for an empty index because a
/// zero `bucket_count` is the shader ABI discriminator for open addressing.
[[nodiscard]] inline GpuIndexMaterializationPlan
plan_gpu_index_materialization(
        std::uint64_t distinct,
        std::uint64_t position_count,
        GpuIndexRepresentation representation) {
    if (representation == GpuIndexRepresentation::sorted_dense
        && (distinct == 0u
            || distinct > (std::numeric_limits<std::uint32_t>::max)())) {
        throw GpuIndexTooLarge{};
    }
    const std::uint64_t slot_capacity =
        representation == GpuIndexRepresentation::sorted_dense
            ? distinct
            : gpu_slot_capacity(distinct);
    const std::uint64_t slot_bytes =
        bioseq::parallelism::saturating_mul(
            slot_capacity, sizeof(GpuMinimizerSlot));
    const std::uint64_t position_bytes =
        bioseq::parallelism::saturating_mul(
            position_count, sizeof(std::uint64_t));
    const std::uint64_t slot_buffer_bytes =
        std::max<std::uint64_t>(slot_bytes, sizeof(GpuMinimizerSlot));
    const std::uint64_t position_buffer_bytes =
        std::max<std::uint64_t>(position_bytes, sizeof(std::uint64_t));
    return {
        .representation = representation,
        .unique_key_count = distinct,
        .slot_capacity = slot_capacity,
        .slot_bytes = slot_bytes,
        .slot_buffer_bytes = slot_buffer_bytes,
        .position_bytes = position_bytes,
        .position_buffer_bytes = position_buffer_bytes,
        .resident_bytes = bioseq::gpu::vulkan::sat_add(
            slot_buffer_bytes, position_buffer_bytes),
        .load_ppm = gpu_index_load_ppm(distinct, slot_capacity),
    };
}

/// Construct the legacy Phase-B open-address table directly in its final mapped
/// Vulkan allocation.  The CPU index remains authoritative; this function
/// allocates nothing, creates no full host upload vectors, and fails closed on
/// malformed buckets, duplicate keys, or a full probe cycle.
[[nodiscard]] inline GpuIndexDirectFillStats direct_fill_gpu_minimizer_index(
        const MinimizerHashmapFragmentIndex& idx,
        std::span<GpuMinimizerSlot> slots,
        std::span<std::uint64_t> positions) {
    const std::uint64_t distinct = idx.table_size();
    const std::uint32_t capacity = gpu_slot_capacity(distinct);
    if (slots.size() != capacity || positions.size() != idx.positions.size()) {
        throw GpuIndexDirectFillInvariant{};
    }
    if (!slots.empty()) {
        std::memset(slots.data(), 0, slots.size_bytes());
    }
    if (!positions.empty()) {
        std::memcpy(positions.data(), idx.positions.data(), positions.size_bytes());
    }

    GpuIndexDirectFillStats stats{
        .unique_key_count = distinct,
        .slot_capacity = capacity,
        .load_ppm = gpu_index_load_ppm(distinct, capacity),
    };
    const std::uint32_t mask = capacity - 1u;
    std::uint64_t inserted = 0u;
    idx.for_each_table_entry([&](std::uint64_t hash, std::uint64_t packed) {
        const std::uint64_t off64 = packed >> 32;
        const std::uint64_t cnt64 = packed & 0xffffffffULL;
        if (cnt64 == 0u
            || off64 > (std::numeric_limits<std::uint32_t>::max)()
            || cnt64 > (std::numeric_limits<std::uint32_t>::max)()
            || off64 > idx.positions.size()
            || cnt64 > idx.positions.size() - off64) {
            throw GpuIndexDirectFillInvariant{};
        }
        std::uint32_t slot_index =
            static_cast<std::uint32_t>(splitmix64(hash)) & mask;
        for (std::uint32_t probe = 1u; probe <= capacity; ++probe) {
            GpuMinimizerSlot& slot = slots[slot_index];
            if (slot.pos_count == 0u) {
                slot.hash = hash;
                slot.pos_offset = static_cast<std::uint32_t>(off64);
                slot.pos_count = static_cast<std::uint32_t>(cnt64);
                stats.max_probe = std::max(stats.max_probe, probe);
                ++inserted;
                return;
            }
            if (slot.hash == hash) {
                throw GpuIndexDirectFillInvariant{};
            }
            slot_index = (slot_index + 1u) & mask;
        }
        throw GpuIndexDirectFillInvariant{};
    });
    if (inserted != distinct || stats.load_ppm > 700'000u) {
        throw GpuIndexDirectFillInvariant{};
    }
    return stats;
}

/// Construct the hash-sorted dense representation directly in the final
/// mapped Vulkan allocation.  The in-place radix sort uses no full-size
/// staging allocation.  Position offsets remain valid because only controls
/// move; the authoritative position array is copied byte-for-byte.
[[nodiscard]] inline GpuIndexDirectFillStats
direct_fill_gpu_minimizer_index_sorted_dense(
        const MinimizerHashmapFragmentIndex& idx,
        std::span<GpuMinimizerSlot> slots,
        std::span<std::uint64_t> positions) {
    const std::uint64_t distinct = idx.table_size();
    if (distinct == 0u
        || distinct > (std::numeric_limits<std::uint32_t>::max)()
        || slots.size() != distinct
        || positions.size() != idx.positions.size()) {
        throw GpuIndexDirectFillInvariant{};
    }
    if (!positions.empty()) {
        std::memcpy(positions.data(), idx.positions.data(), positions.size_bytes());
    }

    std::uint64_t inserted = 0u;
    idx.for_each_table_entry([&](std::uint64_t hash, std::uint64_t packed) {
        const std::uint64_t off64 = packed >> 32;
        const std::uint64_t cnt64 = packed & 0xffffffffULL;
        if (cnt64 == 0u
            || off64 > (std::numeric_limits<std::uint32_t>::max)()
            || cnt64 > (std::numeric_limits<std::uint32_t>::max)()
            || off64 > idx.positions.size()
            || cnt64 > idx.positions.size() - off64
            || inserted >= slots.size()) {
            throw GpuIndexDirectFillInvariant{};
        }
        slots[static_cast<std::size_t>(inserted++)] = GpuMinimizerSlot{
            .hash = hash,
            .pos_offset = static_cast<std::uint32_t>(off64),
            .pos_count = static_cast<std::uint32_t>(cnt64),
        };
    });
    if (inserted != distinct) {
        throw GpuIndexDirectFillInvariant{};
    }
    bioseq::sorting::radix_sort(
        slots.data(), slots.data() + slots.size(),
        [](const GpuMinimizerSlot& slot) noexcept { return slot.hash; });
    for (std::size_t i = 1; i < slots.size(); ++i) {
        if (slots[i - 1u].hash >= slots[i].hash) {
            throw GpuIndexDirectFillInvariant{};
        }
    }
    return {
        .unique_key_count = distinct,
        .slot_capacity = distinct,
        .load_ppm = 1'000'000u,
        .max_probe = 0u,
    };
}

enum class GpuBuiltIndexAdoptionError : std::uint8_t {
    invalid_build,
    missing_ownership,
    evidence_mismatch,
};

[[nodiscard]] inline constexpr std::string_view
gpu_built_index_adoption_error_name(
        GpuBuiltIndexAdoptionError error) noexcept {
    switch (error) {
        case GpuBuiltIndexAdoptionError::invalid_build:
            return "invalid_build";
        case GpuBuiltIndexAdoptionError::missing_ownership:
            return "missing_ownership";
        case GpuBuiltIndexAdoptionError::evidence_mismatch:
            return "evidence_mismatch";
    }
    return "unknown";
}

/// Zero-allocation ownership handoff for a runtime-built index. The original
/// count/main admission lineage either retains an exact-R permit or carries a
/// strict footprint-backed release commit; adoption never reserves again.
[[nodiscard]] inline std::expected<
    std::shared_ptr<GpuResidentMinimizerIndex>, GpuBuiltIndexAdoptionError>
adopt_gpu_built_index(
    bioseq::gpu::vulkan::Runtime& runtime,
    bioseq::gpu::vulkan::GpuBuiltIndex&& built,
    GpuMinimizerIndexCacheKey key)
{
    if (!built.valid) {
        return std::unexpected{GpuBuiltIndexAdoptionError::invalid_build};
    }
    const std::uint64_t required_position_bytes =
        bioseq::parallelism::saturating_mul(built.position_count, 8u);
    const std::uint64_t required_bucket_bytes =
        bioseq::parallelism::saturating_mul(built.occupied_slots, 16u);
    if (!built.slots.valid() || !built.positions.valid()
        || built.slots.owner != &runtime || built.positions.owner != &runtime
        || built.slots.mapped == nullptr || built.positions.mapped == nullptr
        || !built.slots.host_visible_coherent()
        || !built.positions.host_visible_coherent()
        || built.slots.allocation_bytes == 0u
        || built.positions.allocation_bytes == 0u
        || required_position_bytes > built.positions.bytes
        || required_bucket_bytes > built.slots.bytes
        || built.slot_mask != 0u
        || built.occupied_slots > built.position_count
        || built.position_count > built.target_len
        || (built.position_count == 0u) != (built.occupied_slots == 0u)) {
        return std::unexpected{GpuBuiltIndexAdoptionError::invalid_build};
    }
    const std::uint64_t bytes =
        bioseq::gpu::vulkan::sat_add(
            built.slots.allocation_bytes, built.positions.allocation_bytes);
    const auto key_gate_identity = gpu_index_gate_identity_from_key(key);
    if (!built.gate_identity.well_formed()
        || !key_gate_identity.well_formed()
        || !built.gate_identity.exact_equals(key_gate_identity)) {
        return std::unexpected{GpuBuiltIndexAdoptionError::evidence_mismatch};
    }
    const auto& commit = built.materialization;
    if (commit.admitted_bytes != bytes
        || commit.evidence.actual_allocation_bytes != bytes
        || commit.evidence.fully_touched_bytes < bytes
        || !commit.evidence.host_visible
        || !commit.evidence.host_coherent
        || built.ownership.resident_bytes != bytes
        || built.ownership.retained_minimizer_count != built.position_count
        || built.ownership.distinct_minimizer_count != built.occupied_slots
        || built.ownership.peak_admitted_bytes != built.peak_admission_bytes
        || built.ownership.peak_observed_bytes
            > built.ownership.peak_admitted_bytes
        || built.ownership.materialization_admitted_bytes
            != commit.admitted_bytes
        || built.ownership.materialization_actual_allocation_bytes
            != commit.evidence.actual_allocation_bytes
        || built.ownership.materialization_fully_touched_bytes
            != commit.evidence.fully_touched_bytes
        || built.ownership.materialization_reflected_delta_bytes
            != commit.reflected_delta_bytes
        || built.ownership.materialization_permit_released
            != static_cast<std::uint64_t>(commit.permit_released)
        || built.ownership.materialization_host_visible
            != static_cast<std::uint64_t>(commit.evidence.host_visible)
        || built.ownership.materialization_host_coherent
            != static_cast<std::uint64_t>(commit.evidence.host_coherent)
        || built.ownership.materialization_process_footprint_reflected
            != static_cast<std::uint64_t>(
                commit.evidence.process_footprint_reflected)) {
        return std::unexpected{GpuBuiltIndexAdoptionError::evidence_mismatch};
    }
    const bool empty = built.position_count == 0u;
    const bool target_build_route_matches =
        built.ownership.exact_count_dispatches == 1u
        && built.ownership.exact_count_prefix_scan_dispatches == 3u
        && built.ownership.exact_count_control_readbacks == 1u
        && built.ownership.deterministic_tile_emit_dispatches
            == (empty ? 0u : 1u)
        && built.ownership.atomic_append_dispatches == 0u
        && built.ownership.emit_validation_control_readbacks
            == (empty ? 0u : 1u)
        && built.ownership.counted_minimizer_count == built.position_count
        && built.ownership.verified_emit_count == built.position_count
        && built.ownership.count_emit_parity_verified == 1u
        && built.ownership.target_minimizer_capacity_estimate > 0u
        && built.ownership.capacity_estimate_underestimated
            == (built.position_count
                    > built.ownership.target_minimizer_capacity_estimate
                ? 1u : 0u)
        && built.ownership.idle_arena_trim_attempts_before_count == 1u
        && built.ownership.idle_arena_trim_successes_before_count == 1u
        && built.ownership.idle_arena_released_bytes_before_count
            <= built.ownership.pretrim_peak_observed_bytes
        && built.ownership.pretrim_peak_observed_bytes
            <= built.ownership.peak_observed_bytes
        && built.ownership.direct_sequence_arena_copies == 1u
        && built.ownership.host_sequence_capacity_releases_before_main
            == (built.ownership.host_sequence_capacity_released_bytes != 0u
                    ? 1u : 0u)
        && built.ownership.direct_tile_arena_fills == 1u
        && built.ownership.host_tile_staging_elided_bytes != 0u
        && built.ownership.count_phase_host_transient_bytes
            == built.ownership.host_sequence_capacity_released_bytes
        && built.ownership.main_phase_host_transient_bytes == 0u
        && built.ownership.host_transient_excluded_before_main_bytes
            == bioseq::parallelism::saturating_add(
                built.ownership.host_sequence_capacity_released_bytes,
                built.ownership.host_tile_staging_elided_bytes)
        && built.ownership.arena_trimmed_before_permit_shrink == 1u;
    if (!target_build_route_matches) {
        return std::unexpected{GpuBuiltIndexAdoptionError::evidence_mismatch};
    }
    const bool gate_identity_matches =
        static_cast<std::uint64_t>(key.gate_enabled)
                == built.ownership.query_gate_enabled
        && key.gate_word_count == built.ownership.query_gate_word_count
        && key.gate_mask == built.ownership.query_gate_mask
        && key.gate_digest_lo == built.ownership.query_gate_digest_lo
        && key.gate_digest_hi == built.ownership.query_gate_digest_hi;
    const bool gate_dispatches_match = built.ownership.query_gate_enabled != 0u
        ? built.ownership.gate_count_dispatches == 1u
            && built.ownership.ungated_emit_dispatches == 0u
            && (built.position_count == 0u
                ? built.ownership.gated_emit_dispatches == 0u
                : built.ownership.gated_emit_dispatches == 1u)
        : built.ownership.gate_count_dispatches == 0u
            && built.ownership.gated_emit_dispatches == 0u
            && (empty
                ? built.ownership.ungated_emit_dispatches == 0u
                : built.ownership.ungated_emit_dispatches == 1u);
    if (!gate_identity_matches || !gate_dispatches_match) {
        return std::unexpected{GpuBuiltIndexAdoptionError::evidence_mismatch};
    }
    const bool retained = static_cast<bool>(built.permit)
        && built.permit.bytes() == bytes
        && !commit.permit_released;
    const bool strictly_released = !static_cast<bool>(built.permit)
        && commit.permit_released
        && commit.evidence.fully_touched_bytes >= bytes
        && commit.evidence.host_visible
        && commit.evidence.host_coherent
        && commit.evidence.process_footprint_reflected
        && commit.reflected_delta_bytes >= bytes;
    if (!retained && !strictly_released) {
        return std::unexpected{GpuBuiltIndexAdoptionError::missing_ownership};
    }
    if (built.ownership.count_phase_admissions != 1u
        || built.ownership.main_peak_admissions != 1u
        || built.ownership.resident_transfers != 0u
        || built.ownership.adoption_rereserve_attempts != 0u
        || built.ownership.adoption_successes != 0u) {
        return std::unexpected{GpuBuiltIndexAdoptionError::evidence_mismatch};
    }

    // Allocate the small owner object before moving any resource, so a host
    // allocation failure leaves `built` completely intact.
    auto resident = std::make_shared<GpuResidentMinimizerIndex>();
    resident->permit = std::move(built.permit);
    resident->slots = std::move(built.slots);
    resident->positions = std::move(built.positions);
    resident->gate_identity = std::move(built.gate_identity);
    resident->materialization = built.materialization;
    resident->ownership = built.ownership;
    resident->ownership.resident_transfers = 1u;
    resident->ownership.adoption_successes = 1u;
    resident->key = key;
    resident->target_len = built.target_len;
    resident->slot_mask = built.slot_mask;          // 0 for the sorted-bucket form
    resident->position_count = built.position_count;
    resident->bucket_count = built.occupied_slots;  // dense bucket count for binary search
    resident->unique_key_count = built.occupied_slots;
    resident->slot_capacity = built.occupied_slots != 0u
        ? built.occupied_slots
        : static_cast<std::uint64_t>(built.slot_mask) + 1u;
    resident->load_ppm = gpu_index_load_ppm(
        resident->unique_key_count, resident->slot_capacity);
    resident->representation = built.occupied_slots != 0u
        ? GpuIndexRepresentation::sorted_dense
        : GpuIndexRepresentation::open_addressed_70;
    built.valid = false;
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
    const MinimizerHashmapFragmentIndex& idx,
    GpuIndexRepresentation representation =
        GpuIndexRepresentation::open_addressed_70)
{
    return plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(), representation).resident_bytes;
}

/// Direct-fill materialization has no full host upload vectors and no staging
/// buffer.  Resident bytes are admitted separately, so transient upload bytes
/// are exactly zero (returning resident bytes here would double-reserve them).
[[nodiscard]] inline std::uint64_t gpu_index_upload_peak_bytes(
    const MinimizerHashmapFragmentIndex&)
{
    return 0u;
}

/// Batch/wave-scoped GPU minimizer index cache. Owned by the outer query
/// batch/segment wave; uploads each distinct index once, reuses the resident
/// handle across queries that share it, and evicts LRU when the resident byte
/// budget is exceeded.
class GpuMinimizerIndexCache {
public:
    explicit GpuMinimizerIndexCache(std::uint64_t resident_limit_bytes)
        : resident_limit_bytes_(resident_limit_bytes) {}

    enum class GateBindError : std::uint8_t {
        cache_not_empty,
        invalid_identity,
        identity_mismatch,
    };
    enum class BuiltInsertError : std::uint8_t {
        null_handle,
        resident_limit,
        gate_identity_mismatch,
        resident_key_mismatch,
    };

    struct GateIdentity {
        bool enabled = false;
        std::uint32_t word_count = 0;
        std::uint32_t mask = 0;
        std::uint64_t digest_lo = 0;
        std::uint64_t digest_hi = 0;
        // The digest is only a prefilter.  The immutable canonical words are
        // the cache-identity authority, so even a forced 128-bit collision
        // cannot publish or retrieve an index built for another query wave.
        std::shared_ptr<const std::vector<std::uint32_t>> canonical_words{};

        [[nodiscard]] bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity
        runtime_identity() const noexcept {
            return bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity{
                .enabled = enabled,
                .word_count = word_count,
                .mask = mask,
                .digest_lo = digest_lo,
                .digest_hi = digest_hi,
                .canonical_words = canonical_words,
            };
        }

        [[nodiscard]] bool well_formed() const noexcept {
            return runtime_identity().well_formed();
        }

        [[nodiscard]] bool metadata_matches(
            const GpuMinimizerIndexCacheKey& key) const noexcept
        {
            return runtime_identity().exact_equals(
                gpu_index_gate_identity_from_key(key));
        }

        [[nodiscard]] bool operator==(const GateIdentity& other) const {
            if (enabled != other.enabled || word_count != other.word_count
                || mask != other.mask || digest_lo != other.digest_lo
                || digest_hi != other.digest_hi) {
                return false;
            }
            if (!enabled) return true;
            if (!canonical_words || !other.canonical_words) return false;
            if (canonical_words.get() == other.canonical_words.get()) return true;
            return *canonical_words == *other.canonical_words;
        }
    };

    [[nodiscard]] std::expected<void, GateBindError> bind_gate_once(
            GateIdentity identity) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!identity.well_formed()) {
            return std::unexpected{GateBindError::invalid_identity};
        }
        if (gate_binding_) {
            return *gate_binding_ == identity
                ? std::expected<void, GateBindError>{}
                : std::unexpected{GateBindError::identity_mismatch};
        }
        if (!entries_.empty()) {
            return std::unexpected{GateBindError::cache_not_empty};
        }
        gate_binding_ = identity;
        return {};
    }

    [[nodiscard]] std::shared_ptr<const GpuResidentMinimizerIndex> get_or_upload(
        bioseq::gpu::vulkan::Runtime& runtime,
        const MinimizerHashmapFragmentIndex& idx,
        const GpuMinimizerIndexCacheKey& key,
        bool cache_enabled,
        GpuIndexUploadDecline* decline_out = nullptr)
    {
        if (decline_out != nullptr) {
            *decline_out = GpuIndexUploadDecline{};
        }
        std::lock_guard<std::mutex> guard(mu_);
        validate_key_gate_locked(key);
        if (cache_enabled) {
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                touch_lru_locked(key);
                return found->second;
            }
        }

        std::shared_ptr<const GpuResidentMinimizerIndex> handle;
        try {
            handle = upload_locked_index(runtime, idx, key, decline_out);
        } catch (const GpuIndexTooLarge&) {
            if (decline_out != nullptr) {
                decline_out->reason =
                    GpuIndexUploadDeclineReason::device_abi_overflow;
                return nullptr;
            }
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan minimizer index exceeds the 32-bit device ABI");
        } catch (const GpuIndexDirectFillInvariant&) {
            if (decline_out != nullptr) {
                decline_out->reason = GpuIndexUploadDeclineReason::
                    direct_fill_invariant_failed;
                return nullptr;
            }
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan minimizer index direct-fill invariant failed");
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
    [[nodiscard]] std::expected<void, BuiltInsertError> insert_built(
        const GpuMinimizerIndexCacheKey& key,
        std::shared_ptr<const GpuResidentMinimizerIndex> handle)
    {
        if (!handle) {
            return std::unexpected{BuiltInsertError::null_handle};
        }
        std::lock_guard<std::mutex> guard(mu_);
        if (!key_gate_matches_binding_locked(key)) {
            return std::unexpected{
                BuiltInsertError::gate_identity_mismatch};
        }
        const auto key_gate_identity = gpu_index_gate_identity_from_key(key);
        if (!handle->gate_identity.well_formed()
            || !handle->gate_identity.exact_equals(key_gate_identity)
            || (gate_binding_
                && !handle->gate_identity.exact_equals(
                    gate_binding_->runtime_identity()))) {
            return std::unexpected{
                BuiltInsertError::gate_identity_mismatch};
        }
        if (!(handle->key == key)) {
            return std::unexpected{BuiltInsertError::resident_key_mismatch};
        }
        const std::uint64_t handle_bytes = handle_resident_bytes(*handle);
        if (resident_limit_bytes_ != 0u
            && handle_bytes > resident_limit_bytes_) {
            return std::unexpected{BuiltInsertError::resident_limit};
        }
        insert_with_eviction_locked(key, handle);
        return {};
    }

    [[nodiscard]] std::shared_ptr<const GpuResidentMinimizerIndex> insert_upload(
        bioseq::gpu::vulkan::Runtime& runtime,
        GpuMinimizerIndexUpload&& upload,
        const GpuMinimizerIndexCacheKey& key,
        bool cache_enabled)
    {
        std::lock_guard<std::mutex> guard(mu_);
        validate_key_gate_locked(key);
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
        gate_binding_.reset();
    }

    [[nodiscard]] std::uint64_t resident_bytes() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        return resident_bytes_;
    }

private:
    [[nodiscard]] bool key_gate_matches_binding_locked(
        const GpuMinimizerIndexCacheKey& key) const noexcept
    {
        if (gate_binding_) {
            return gate_binding_->well_formed()
                && gate_binding_->metadata_matches(key);
        }
        // A gate-enabled key without an exact canonical binding would make a
        // digest authoritative.  Fail it closed.  The ordinary ungated cache
        // remains usable without an explicit bind.
        return !key.gate_enabled && key.gate_word_count == 0u
            && key.gate_mask == 0u && key.gate_digest_lo == 0u
            && key.gate_digest_hi == 0u && !key.gate_canonical_words;
    }

    void validate_key_gate_locked(
        const GpuMinimizerIndexCacheKey& key) const
    {
        if (!key_gate_matches_binding_locked(key)) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan minimizer-index cache gate identity mismatch");
        }
    }

    [[nodiscard]] static std::uint64_t handle_resident_bytes(
        const GpuResidentMinimizerIndex& handle) noexcept
    {
        return bioseq::gpu::vulkan::sat_add(
            handle.slots.allocation_bytes,
            handle.positions.allocation_bytes);
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
        // `insert_built()` may publish a replacement under a key that is
        // already resident. Remove the previous accounting and every stale
        // LRU occurrence before considering the new allocation; otherwise a
        // replacement double-counts bytes and can later evict the wrong handle.
        if (auto existing = entries_.find(key); existing != entries_.end()) {
            const std::uint64_t existing_bytes =
                handle_resident_bytes(*existing->second);
            resident_bytes_ = existing_bytes <= resident_bytes_
                ? resident_bytes_ - existing_bytes
                : 0u;
            entries_.erase(existing);
            lru_.remove(key);
        }
        // A handle larger than the whole budget is used uncached (already
        // returned to the caller); do not pollute the cache with it.
        if (resident_limit_bytes_ != 0 && handle_bytes > resident_limit_bytes_) {
            return;
        }
        const auto exceeds_resident_limit = [&]() noexcept {
            return resident_limit_bytes_ != 0
                && (resident_bytes_ > resident_limit_bytes_
                    || handle_bytes
                        > resident_limit_bytes_ - resident_bytes_);
        };
        while (!lru_.empty() && exceeds_resident_limit()) {
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
        resident_bytes_ = bioseq::gpu::vulkan::sat_add(
            resident_bytes_, handle_bytes);
    }

    [[nodiscard]] std::shared_ptr<const GpuResidentMinimizerIndex> upload_locked_index(
        bioseq::gpu::vulkan::Runtime& runtime,
        const MinimizerHashmapFragmentIndex& idx,
        const GpuMinimizerIndexCacheKey& key,
        GpuIndexUploadDecline* decline_out)
    {
        using bioseq::parallelism::MemoryAdmissionController;
        if (idx.positions.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
            throw GpuIndexTooLarge{};
        }
        const std::uint64_t upload_peak = gpu_index_upload_peak_bytes(idx);
        const std::uint64_t unique_key_count = idx.table_size();
        const GpuIndexMaterializationPlan open_plan =
            plan_gpu_index_materialization(
                unique_key_count, idx.positions.size(),
                GpuIndexRepresentation::open_addressed_70);
        std::optional<GpuIndexMaterializationPlan> dense_plan;
        if (unique_key_count != 0u
            && unique_key_count
                <= (std::numeric_limits<std::uint32_t>::max)()) {
            dense_plan = plan_gpu_index_materialization(
                unique_key_count, idx.positions.size(),
                GpuIndexRepresentation::sorted_dense);
        }
        const std::uint64_t max_storage_buffer_range =
            runtime.features().max_storage_buffer_range;
        const auto ranges_fit = [max_storage_buffer_range](
                const GpuIndexMaterializationPlan& candidate) noexcept {
            return gpu_index_storage_ranges_fit(
                candidate.slot_buffer_bytes,
                candidate.position_buffer_bytes,
                max_storage_buffer_range);
        };
        GpuIndexMaterializationPlan plan = open_plan;
        if (!ranges_fit(open_plan) && dense_plan) {
            plan = *dense_plan;
        }
        const auto publish_plan = [&](
                const GpuIndexMaterializationPlan& selected) {
            if (decline_out == nullptr) return;
            decline_out->resident_bytes = selected.resident_bytes;
            decline_out->upload_peak_bytes = upload_peak;
            decline_out->slot_bytes = selected.slot_bytes;
            decline_out->position_bytes = selected.position_bytes;
            decline_out->unique_key_count = selected.unique_key_count;
            decline_out->slot_capacity = selected.slot_capacity;
            decline_out->load_ppm = selected.load_ppm;
            decline_out->representation = selected.representation;
            decline_out->staging_bytes = 0u;
            decline_out->max_storage_buffer_range =
                max_storage_buffer_range;
        };
        publish_plan(plan);

        if (!ranges_fit(plan)) {
            if (decline_out != nullptr) {
                decline_out->reason = GpuIndexUploadDeclineReason::
                    storage_buffer_range_exceeded;
            }
            return nullptr;
        }

        auto& admission_controller = MemoryAdmissionController::instance();
        bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
        auto resident_permit =
            admission_controller.try_reserve(
                plan.resident_bytes, &admission_failure);
        if (!resident_permit
            && plan.representation
                == GpuIndexRepresentation::open_addressed_70
            && dense_plan
            && dense_plan->resident_bytes < plan.resident_bytes
            && ranges_fit(*dense_plan)) {
            const auto open_failure = admission_failure;
            plan = *dense_plan;
            publish_plan(plan);
            admission_failure = {};
            bioseq_log<INF>(
                "[gpu_index_representation_adaptation] from={} to={} "
                "open_resident_bytes={} dense_resident_bytes={} "
                "open_admission_reason={} open_budget_ceiling_bytes={} "
                "open_allocatable_after_reservations_bytes={}",
                gpu_index_representation_name(
                    GpuIndexRepresentation::open_addressed_70),
                gpu_index_representation_name(
                    GpuIndexRepresentation::sorted_dense),
                open_plan.resident_bytes,
                plan.resident_bytes,
                bioseq::parallelism::memory_admission_failure_reason_name(
                    open_failure.reason),
                open_failure.budget_ceiling_bytes,
                open_failure.allocatable_after_reservations_bytes);
            resident_permit = admission_controller.try_reserve(
                plan.resident_bytes, &admission_failure);
        }
        if (!resident_permit) {
            if (decline_out != nullptr) {
                decline_out->reason = GpuIndexUploadDeclineReason::
                    resident_memory_admission_denied;
                decline_out->admission = admission_failure;
            }
            return nullptr;
        }

        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto resident = std::make_shared<GpuResidentMinimizerIndex>();
        GpuIndexDirectFillStats fill_stats{};
        bool allocation_growth_denied = false;
        std::uint64_t denied_actual_allocation_bytes = 0u;
        bool non_host_visible = false;
        bioseq::parallelism::MemoryAdmissionFailure growth_failure{};
        const auto materialization = admission_controller.materialize(
            *resident_permit,
            [&]() -> bioseq::parallelism::MemoryMaterializationEvidence {
                resident->slots = runtime.create_device_buffer(
                    plan.slot_buffer_bytes, usage);
                resident->positions = runtime.create_device_buffer(
                    plan.position_buffer_bytes, usage);
                const std::uint64_t actual_allocation_bytes =
                    bioseq::gpu::vulkan::sat_add(
                        resident->slots.allocation_bytes,
                        resident->positions.allocation_bytes);
                if (!resident_permit->grow_to(
                        actual_allocation_bytes, &growth_failure)) {
                    allocation_growth_denied = true;
                    denied_actual_allocation_bytes = actual_allocation_bytes;
                    resident->positions.reset();
                    resident->slots.reset();
                    return {};
                }
                if (resident->slots.mapped == nullptr
                    || resident->positions.mapped == nullptr) {
                    non_host_visible = true;
                    return {
                        .actual_allocation_bytes = actual_allocation_bytes,
                        .host_visible = false,
                        .host_coherent = false,
                        .process_footprint_reflected = false,
                    };
                }

                const auto mapped_slots = std::span<GpuMinimizerSlot>{
                    static_cast<GpuMinimizerSlot*>(resident->slots.mapped),
                    static_cast<std::size_t>(plan.slot_capacity)};
                const auto mapped_positions = std::span<std::uint64_t>{
                    static_cast<std::uint64_t*>(resident->positions.mapped),
                    idx.positions.size()};
                fill_stats = plan.representation
                        == GpuIndexRepresentation::sorted_dense
                    ? direct_fill_gpu_minimizer_index_sorted_dense(
                        idx, mapped_slots, mapped_positions)
                    : direct_fill_gpu_minimizer_index(
                        idx, mapped_slots, mapped_positions);
                if (resident->slots.allocation_bytes > plan.slot_bytes) {
                    std::memset(
                        static_cast<std::byte*>(resident->slots.mapped)
                            + plan.slot_bytes,
                        0,
                        static_cast<std::size_t>(
                            resident->slots.allocation_bytes - plan.slot_bytes));
                }
                if (resident->positions.allocation_bytes > plan.position_bytes) {
                    std::memset(
                        static_cast<std::byte*>(resident->positions.mapped)
                            + plan.position_bytes,
                        0,
                        static_cast<std::size_t>(resident->positions.allocation_bytes
                            - plan.position_bytes));
                }
                const bool host_visible =
                    (resident->slots.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u
                    && (resident->positions.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u;
                const bool host_coherent =
                    (resident->slots.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u
                    && (resident->positions.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
                return {
                    .actual_allocation_bytes = actual_allocation_bytes,
                    .fully_touched_bytes = actual_allocation_bytes,
                    .host_visible = host_visible,
                    .host_coherent = host_coherent,
                    .process_footprint_reflected =
                        resident->slots.process_footprint_reflected
                        && resident->positions.process_footprint_reflected,
                };
            });
        if (allocation_growth_denied) {
            if (decline_out != nullptr) {
                decline_out->reason = GpuIndexUploadDeclineReason::
                    resident_memory_admission_denied;
                decline_out->resident_bytes =
                    denied_actual_allocation_bytes;
                decline_out->actual_allocation_bytes =
                    denied_actual_allocation_bytes;
                decline_out->admission = growth_failure;
            }
            return nullptr;
        }
        if (non_host_visible) {
            if (decline_out != nullptr) {
                decline_out->reason = GpuIndexUploadDeclineReason::
                    non_host_visible_device_memory;
            }
            return nullptr;
        }
        resident->permit = std::move(*resident_permit);
        resident->key = key;
        resident->gate_identity = gpu_index_gate_identity_from_key(key);
        resident->target_len = idx.target_len;
        resident->slot_mask = plan.representation
                == GpuIndexRepresentation::open_addressed_70
            ? static_cast<std::uint32_t>(plan.slot_capacity - 1u)
            : 0u;
        resident->position_count = static_cast<std::uint32_t>(idx.positions.size());
        resident->bucket_count = plan.representation
                == GpuIndexRepresentation::sorted_dense
            ? static_cast<std::uint32_t>(plan.slot_capacity)
            : 0u;
        resident->unique_key_count = fill_stats.unique_key_count;
        resident->slot_capacity = fill_stats.slot_capacity;
        resident->load_ppm = fill_stats.load_ppm;
        resident->max_probe = fill_stats.max_probe;
        resident->representation = plan.representation;
        resident->staging_bytes = 0u;
        resident->materialization = materialization;
        if (decline_out != nullptr) {
            decline_out->max_probe = fill_stats.max_probe;
            decline_out->actual_allocation_bytes =
                materialization.evidence.actual_allocation_bytes;
            decline_out->fully_touched_bytes =
                materialization.evidence.fully_touched_bytes;
            decline_out->reflected_footprint_delta_bytes =
                materialization.reflected_delta_bytes;
            decline_out->materialization_permit_released =
                materialization.permit_released;
            decline_out->process_footprint_reflected =
                materialization.evidence.process_footprint_reflected;
        }
        bioseq_log<INF>(
            "[gpu_index_materialization_success] representation={} unique_keys={} "
            "slot_capacity={} load_ppm={} max_probe={} logical_bytes={} "
            "actual_allocation_bytes={} fully_touched_bytes={} "
            "staging_bytes=0 permit_released={} footprint_reflected={} "
            "reflected_delta_bytes={}",
            gpu_index_representation_name(plan.representation),
            fill_stats.unique_key_count,
            fill_stats.slot_capacity,
            fill_stats.load_ppm,
            fill_stats.max_probe,
            plan.resident_bytes,
            materialization.evidence.actual_allocation_bytes,
            materialization.evidence.fully_touched_bytes,
            materialization.permit_released ? 1 : 0,
            materialization.evidence.process_footprint_reflected ? 1 : 0,
            materialization.reflected_delta_bytes);
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
        resident->gate_identity = gpu_index_gate_identity_from_key(key);
        resident->target_len = upload.target_len;
        resident->slot_mask = upload.slot_mask;
        resident->position_count = static_cast<std::uint32_t>(upload.positions.size());
        resident->bucket_count = 0u;
        return resident;
    }

    std::uint64_t resident_bytes_ = 0;
    std::uint64_t resident_limit_bytes_ = 0;
    std::optional<GateIdentity> gate_binding_{};
    mutable std::mutex mu_;
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
    invalid_stream_config,
    invalid_stream_phase,
    slot_busy,
    stale_slot_handle,
    completion_mismatch,
    unauthorized_extent,
    poisoned_stream,
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
    std::uint64_t grouped32_control_groups_opened = 0;
    std::uint64_t grouped32_control_groups_completed = 0;
    std::uint64_t grouped32_position_groups_opened = 0;
    std::uint64_t grouped32_position_groups_completed = 0;
    std::uint64_t grouped32_control_extents_validated = 0;
    std::uint64_t grouped32_position_extents_validated = 0;
    std::uint64_t grouped32_control_bytes_streamed = 0;
    std::uint64_t grouped32_position_bytes_streamed = 0;
    std::uint64_t grouped32_slot_reuses = 0;
    std::uint64_t grouped32_max_inflight_slots = 0;
    std::uint64_t grouped32_authorized_partitions = 0;
    std::uint64_t grouped32_authorized_anchors = 0;
    std::uint64_t grouped32_target_seals = 0;

    std::uint64_t forbidden_host_query_hash_materializations = 0;
    std::uint64_t forbidden_host_pair_materializations = 0;
    std::uint64_t forbidden_host_pair_sorts = 0;
    std::uint64_t forbidden_host_control_rebases = 0;
    std::uint64_t forbidden_host_anchor_concatenations = 0;
    std::uint64_t forbidden_host_grouped32_control_decodes = 0;
    std::uint64_t forbidden_host_grouped32_position_decodes = 0;
    std::uint64_t forbidden_host_grouped32_position_globalizations = 0;
    std::uint64_t forbidden_host_sorted_dense_transcodes = 0;
    std::uint64_t forbidden_host_global64_materializations = 0;
    std::uint64_t forbidden_cpu_or_simd_fallbacks = 0;
    bool saturated = false;

    [[nodiscard]] bool has_forbidden_work() const noexcept {
        return forbidden_host_query_hash_materializations != 0u
            || forbidden_host_pair_materializations != 0u
            || forbidden_host_pair_sorts != 0u
            || forbidden_host_control_rebases != 0u
            || forbidden_host_anchor_concatenations != 0u
            || forbidden_host_grouped32_control_decodes != 0u
            || forbidden_host_grouped32_position_decodes != 0u
            || forbidden_host_grouped32_position_globalizations != 0u
            || forbidden_host_sorted_dense_transcodes != 0u
            || forbidden_host_global64_materializations != 0u
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
    std::uint32_t gpu_abi_version = 0;

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
    const ExpectedMinimizerCompatibility& expected,
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
    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

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
        const ExpectedMinimizerCompatibility&,
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
    const ExpectedMinimizerCompatibility& expected,
    const DirView& directory,
    GpuPersistedIndexLocalProof& proof)
{
    static_assert(MinimizerBucketDirEntry::encoded_bytes
        == bioseq::gpu::vulkan::k_gpu_persisted_directory_entry_bytes);
    if (auto compatible = validate_minimizer_generation_compatibility(
            manifest, expected); !compatible) {
        return std::unexpected(
            GpuPersistedIndexFoundationError::metadata_mismatch);
    }
    if (!is_supported_minimizer_generation_wire_format(
            directory.profile().wire_format)) {
        return std::unexpected(
            GpuPersistedIndexFoundationError::metadata_mismatch);
    }
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
    const std::uint64_t requested_bytes =
        std::max<std::uint64_t>(encoded_bytes, sizeof(std::uint32_t));
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const std::uint64_t admitted_allocation_bytes =
        runtime.buffer_allocation_requirement(requested_bytes, usage);
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(admitted_allocation_bytes);
    if (!permit) {
        return std::unexpected(GpuPersistedIndexFoundationError::admission);
    }

    bioseq::gpu::vulkan::Buffer encoded_directory =
        runtime.create_device_buffer(requested_bytes, usage);
    if (encoded_directory.allocation_bytes > admitted_allocation_bytes) {
        return std::unexpected(GpuPersistedIndexFoundationError::admission);
    }
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
        .gpu_abi_version = manifest.gpu_abi_version,
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
        || (identity.gpu_abi_version
                != kMinimizerSortedDenseGpuAbiVersion
            && identity.gpu_abi_version
                != kMinimizerGrouped32GpuAbiVersion)
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
        const ExpectedMinimizerCompatibility& expected,
        std::uint32_t ordinal)
    {
        if (!validate_minimizer_generation_compatibility(
                manifest, expected)) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::metadata_mismatch);
        }
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
            && directory_identity.gpu_abi_version
                == manifest.gpu_abi_version
            && directory_identity.device_uuid == runtime.features().device_uuid;
        if (!selected
            || !std::binary_search(selected->begin(), selected->end(), ordinal)
            || entry == nullptr || !directory_matches) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::selection_identity_mismatch);
        }
        const auto entry_error = validate_minimizer_bucket_dir_entry(
            *entry, authoritative_directory.profile());
        if (!valid_gpu_persisted_cache_key(key)
            || key.device_uuid != runtime.features().device_uuid
            || key.generation_id != manifest.generation_id
            || key.source_identity != manifest.source_identity
            || manifest.gpu_abi_version
                != kMinimizerSortedDenseGpuAbiVersion
            || authoritative_directory.profile().wire_format
                != MinimizerGenerationWireFormat::sorted_dense_global64
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

// ===========================================================================
// Format-5 / ABI3 full-generation cold resident (scheduler routing follows)
// ===========================================================================

static_assert(kMinimizerShapeV5GpuAbiVersion
    == bioseq::gpu::vulkan::k_gpu_shape_v5_abi_version);

/// Immutable identity of one restart-cold Format-5 upload.  The opaque
/// cold_load_identity is supplied by the PostgreSQL/scheduler owner; it is not
/// derived from a host pointer or from query contents.
struct GpuShapeV5ResidentIdentity {
    MinimizerGenerationWireFormat wire_format =
        MinimizerGenerationWireFormat::invalid;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_checksum = 0u;
    std::uint64_t compatibility_checksum = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t directory_checksum = 0u;
    std::uint64_t payload_checksum = 0u;
    std::uint64_t cold_load_identity = 0u;
    std::uint64_t directory_bytes = 0u;
    std::uint64_t control_bytes = 0u;
    std::uint64_t position_bytes = 0u;
    std::uint64_t descriptor_bytes = 0u;
    std::uint64_t control_count = 0u;
    std::uint64_t position_count = 0u;
    std::uint32_t descriptor_count = 0u;
    GpuDeviceUuid device_uuid{};
    std::uint32_t gpu_abi_version = 0u;

    [[nodiscard]] bool operator==(
        const GpuShapeV5ResidentIdentity&) const = default;
};

[[nodiscard]] inline bool valid_gpu_shape_v5_resident_identity(
    const GpuShapeV5ResidentIdentity& identity) noexcept
{
    return identity.wire_format
            == MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local
        && identity.generation_id != 0u
        && identity.source_identity != 0u
        && identity.cold_load_identity != 0u
        && gpu_device_uuid_is_bound(identity.device_uuid)
        && identity.gpu_abi_version == kMinimizerShapeV5GpuAbiVersion;
}

/// Positive and prohibited-work evidence owned by a completed cold upload.
/// Counts describe bytes copied directly into their final device buffers; no
/// decoded host index or temporary pair array is part of this representation.
struct GpuShapeV5ColdLoadProof {
    std::uint64_t cold_cache_misses = 0u;
    std::uint64_t full_generations_ingested = 0u;
    std::uint64_t control_extents_validated = 0u;
    std::uint64_t position_extents_validated = 0u;
    std::uint64_t control_bytes_copied = 0u;
    std::uint64_t position_bytes_copied = 0u;
    std::uint64_t descriptor_bytes_copied = 0u;
    std::uint64_t device_allocation_bytes = 0u;
    std::uint64_t admitted_device_bytes = 0u;
    std::uint64_t validated_receipt_rows = 0u;
    std::uint64_t raw_rehashed_rows = 0u;
    std::uint64_t payload_hash_byte_visits = 0u;
    std::uint64_t forbidden_host_control_decodes = 0u;
    std::uint64_t forbidden_host_position_decodes = 0u;
    std::uint64_t forbidden_host_position_globalizations = 0u;
    std::uint64_t forbidden_host_index_rebuilds = 0u;
    std::uint64_t forbidden_cpu_or_simd_fallbacks = 0u;

    [[nodiscard]] bool has_forbidden_work() const noexcept {
        return forbidden_host_control_decodes != 0u
            || forbidden_host_position_decodes != 0u
            || forbidden_host_position_globalizations != 0u
            || forbidden_host_index_rebuilds != 0u
            || forbidden_cpu_or_simd_fallbacks != 0u;
    }
};

class GpuShapeV5FullGenerationIngest;

/// Cacheable, immutable Format-5 target.  `controls` retains each persisted
/// SoA extent byte-for-byte and `positions` retains the persisted segment-local
/// scalar lanes byte-for-byte.  The descriptor buffer is the only derived
/// device object and contains metadata/byte offsets, never decoded payload.
class GpuResidentShapeV5Target {
private:
    struct ConstructionToken {};

public:
    GpuResidentShapeV5Target(
        ConstructionToken,
        bioseq::parallelism::MemoryPermit permit,
        bioseq::gpu::vulkan::Buffer controls,
        bioseq::gpu::vulkan::Buffer positions,
        bioseq::gpu::vulkan::Buffer descriptors,
        GpuShapeV5ResidentIdentity identity,
        std::uint64_t directory_bytes,
        std::uint64_t control_bytes,
        std::uint64_t position_bytes,
        std::uint64_t descriptor_bytes,
        std::uint64_t control_count,
        std::uint64_t position_count,
        std::uint32_t descriptor_count) noexcept
        : permit_(std::move(permit)),
          controls_(std::move(controls)),
          positions_(std::move(positions)),
          descriptors_(std::move(descriptors)),
          identity_(identity),
          directory_bytes_(directory_bytes),
          control_bytes_(control_bytes),
          position_bytes_(position_bytes),
          descriptor_bytes_(descriptor_bytes),
          control_count_(control_count),
          position_count_(position_count),
          descriptor_count_(descriptor_count) {}

    [[nodiscard]] const bioseq::gpu::vulkan::Buffer& controls() const noexcept {
        return controls_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::Buffer& positions() const noexcept {
        return positions_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::Buffer& descriptors() const noexcept {
        return descriptors_;
    }
    [[nodiscard]] const GpuShapeV5ResidentIdentity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const GpuShapeV5ColdLoadProof& cold_load_proof() const noexcept {
        return proof_;
    }
    [[nodiscard]] std::uint64_t admitted_device_bytes() const noexcept {
        return permit_.bytes();
    }
    [[nodiscard]] std::uint64_t directory_bytes() const noexcept {
        return directory_bytes_;
    }
    [[nodiscard]] std::uint64_t control_bytes() const noexcept {
        return control_bytes_;
    }
    [[nodiscard]] std::uint64_t position_bytes() const noexcept {
        return position_bytes_;
    }
    [[nodiscard]] std::uint64_t descriptor_bytes() const noexcept {
        return descriptor_bytes_;
    }
    [[nodiscard]] std::uint64_t control_count() const noexcept {
        return control_count_;
    }
    [[nodiscard]] std::uint64_t position_count() const noexcept {
        return position_count_;
    }
    [[nodiscard]] std::uint32_t descriptor_count() const noexcept {
        return descriptor_count_;
    }

    /// This slice is an index-residency proof, not a scheduler execution proof.
    [[nodiscard]] static constexpr bool
    qualifies_as_all_gpu_execution() noexcept { return false; }
    [[nodiscard]] static constexpr bool
    permits_host_decode_or_rebuild() noexcept { return false; }

    [[nodiscard]] bool validated() const noexcept {
        const std::uint64_t payload_bytes =
            bioseq::parallelism::saturating_add(
                control_bytes_, position_bytes_);
        const std::uint64_t expected_directory_bytes =
            bioseq::parallelism::saturating_mul(
                descriptor_count_,
                MinimizerBucketDirEntry::format5_encoded_bytes);
        const std::uint64_t expected_descriptor_bytes =
            bioseq::parallelism::saturating_mul(
                descriptor_count_,
                sizeof(bioseq::gpu::vulkan::
                    GpuShapeV5ResidentPartitionDescriptor));
        const std::uint64_t allocation_bytes =
            bioseq::parallelism::saturating_add(
                controls_.allocation_bytes,
                bioseq::parallelism::saturating_add(
                    positions_.allocation_bytes,
                    descriptors_.allocation_bytes));
        return valid_gpu_shape_v5_resident_identity(identity_)
            && controls_.valid() && positions_.valid() && descriptors_.valid()
            && payload_bytes
                != (std::numeric_limits<std::uint64_t>::max)()
            && expected_directory_bytes == directory_bytes_
            && expected_descriptor_bytes == descriptor_bytes_
            && identity_.directory_bytes == directory_bytes_
            && identity_.control_bytes == control_bytes_
            && identity_.position_bytes == position_bytes_
            && identity_.descriptor_bytes == descriptor_bytes_
            && identity_.control_count == control_count_
            && identity_.position_count == position_count_
            && identity_.descriptor_count == descriptor_count_
            && control_bytes_ <= controls_.bytes
            && position_bytes_ <= positions_.bytes
            && descriptor_bytes_ <= descriptors_.bytes
            && allocation_bytes
                != (std::numeric_limits<std::uint64_t>::max)()
            && allocation_bytes == permit_.bytes()
            && proof_.cold_cache_misses == 1u
            && proof_.full_generations_ingested == 1u
            && proof_.control_extents_validated == descriptor_count_
            && proof_.position_extents_validated == descriptor_count_
            && proof_.control_bytes_copied == control_bytes_
            && proof_.position_bytes_copied == position_bytes_
            && proof_.descriptor_bytes_copied == descriptor_bytes_
            && proof_.device_allocation_bytes == allocation_bytes
            && proof_.admitted_device_bytes == permit_.bytes()
            && proof_.validated_receipt_rows != 0u
            && proof_.raw_rehashed_rows == 0u
            && proof_.payload_hash_byte_visits == 0u
            && !proof_.has_forbidden_work();
    }

private:
    // First declaration ensures buffers are destroyed before their admission.
    bioseq::parallelism::MemoryPermit permit_{};
    bioseq::gpu::vulkan::Buffer controls_{};
    bioseq::gpu::vulkan::Buffer positions_{};
    bioseq::gpu::vulkan::Buffer descriptors_{};
    GpuShapeV5ResidentIdentity identity_{};
    GpuShapeV5ColdLoadProof proof_{};
    std::uint64_t directory_bytes_ = 0u;
    std::uint64_t control_bytes_ = 0u;
    std::uint64_t position_bytes_ = 0u;
    std::uint64_t descriptor_bytes_ = 0u;
    std::uint64_t control_count_ = 0u;
    std::uint64_t position_count_ = 0u;
    std::uint32_t descriptor_count_ = 0u;

    friend class GpuShapeV5FullGenerationIngest;
};

/// Direct full-generation receiver for the first restart-cold measurement.
/// Requests follow PostgreSQL's physical Format-5 order: every control extent,
/// then every position extent. Chunks are copied once into final resident
/// buffers. PostgreSQL's validated-row receipts supply already-checked row
/// checksums, avoiding a second payload-byte hash pass. The raw entry point is
/// deliberately test-only and cannot publish a resident.
class GpuShapeV5FullGenerationIngest {
public:
    GpuShapeV5FullGenerationIngest(
        const GpuShapeV5FullGenerationIngest&) = delete;
    GpuShapeV5FullGenerationIngest& operator=(
        const GpuShapeV5FullGenerationIngest&) = delete;
    GpuShapeV5FullGenerationIngest(
        GpuShapeV5FullGenerationIngest&&) noexcept = default;
    GpuShapeV5FullGenerationIngest& operator=(
        GpuShapeV5FullGenerationIngest&&) = delete;

    [[nodiscard]] static std::expected<
        GpuShapeV5FullGenerationIngest,
        GpuPersistedIndexFoundationError>
    begin(
        bioseq::gpu::vulkan::Runtime& runtime,
        const MinimizerGenerationHead& head,
        const MinimizerGenerationManifest& manifest,
        const ExpectedMinimizerCompatibility& expected,
        const DirView& directory,
        std::uint64_t cold_load_identity)
    {
        if constexpr (std::endian::native != std::endian::little) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::unsupported_endianness);
        }
        if (!runtime.usable()
            || !gpu_device_uuid_is_bound(runtime.features().device_uuid)
            || cold_load_identity == 0u) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::invalid_identity);
        }
        const bool format5 = manifest.wire_format
                == MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local
            && manifest.gpu_abi_version == kMinimizerShapeV5GpuAbiVersion
            && directory.profile().wire_format
                == MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local
            && directory.profile().gpu_abi_version
                == kMinimizerShapeV5GpuAbiVersion
            && manifest.shape_v5.has_value()
            && directory.profile().shape_v5 == manifest.shape_v5;
        if (!format5
            || !validate_minimizer_generation_compatibility(
                manifest, expected)
            || !validate_minimizer_generation_metadata(
                head, manifest, directory)) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::metadata_mismatch);
        }
        const auto entries = directory.entries();
        if (entries.size()
                > (std::numeric_limits<std::uint32_t>::max)()
            || entries.size()
                > (std::numeric_limits<std::size_t>::max)() / 2u) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::arithmetic_overflow);
        }

        std::vector<std::uint64_t> control_offsets(entries.size());
        std::vector<std::uint64_t> position_offsets(entries.size());
        std::vector<bioseq::gpu::vulkan::
            GpuShapeV5ResidentPartitionDescriptor> descriptors;
        descriptors.reserve(entries.size());
        std::uint64_t control_bytes = 0u;
        std::uint64_t position_bytes = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_count = 0u;
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            const MinimizerBucketDirEntry& entry = entries[ordinal];
            if (validate_minimizer_bucket_dir_entry(
                    entry, directory.profile()).has_value()
                || entry.partition_id != ordinal
                || entry.control_element_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                || entry.position_element_count
                    > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::partition_mismatch);
            }
            const auto core = minimizer_position_segment_core(
                manifest, static_cast<std::uint32_t>(entry.position_segment));
            const auto layout = minimizer_shape_v5_control_layout(
                entry.control_element_count,
                manifest.shape_v5->hash_width,
                entry.shape_offset_width,
                manifest.shape_v5->count_width);
            if (!core || !layout
                || control_bytes
                    % static_cast<std::uint8_t>(
                        manifest.shape_v5->hash_width) != 0u
                || position_bytes
                    % static_cast<std::uint8_t>(
                        manifest.shape_v5->position_width) != 0u
                || control_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.control_encoded_bytes
                || position_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.position_encoded_bytes
                || control_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.control_element_count
                || position_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.position_element_count) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::arithmetic_overflow);
            }
            control_offsets[ordinal] = control_bytes;
            position_offsets[ordinal] = position_bytes;
            descriptors.push_back(
                bioseq::gpu::vulkan::GpuShapeV5ResidentPartitionDescriptor{
                    .partition_id = entry.partition_id,
                    .directory_ordinal = static_cast<std::uint32_t>(ordinal),
                    .position_segment = static_cast<std::uint32_t>(
                        entry.position_segment),
                    .flags = bioseq::gpu::vulkan::
                        k_gpu_shape_v5_partition_required_flags,
                    .hash_byte_offset = control_bytes + layout->hash_offset,
                    .prefix_byte_offset =
                        control_bytes + layout->prefix_offset,
                    .count_byte_offset = control_bytes + layout->count_offset,
                    .position_byte_offset = position_bytes,
                    .control_count = static_cast<std::uint32_t>(
                        entry.control_element_count),
                    .position_count = static_cast<std::uint32_t>(
                        entry.position_element_count),
                    .hash_width = static_cast<std::uint32_t>(
                        manifest.shape_v5->hash_width),
                    .offset_width = static_cast<std::uint32_t>(
                        entry.shape_offset_width),
                    .count_width = static_cast<std::uint32_t>(
                        manifest.shape_v5->count_width),
                    .position_width = static_cast<std::uint32_t>(
                        manifest.shape_v5->position_width),
                    .low_hash = entry.lo_hash,
                    .high_hash = entry.hi_hash,
                    .segment_base_bp = core->begin_bp,
                    .segment_end_bp = core->end_bp,
                });
            control_bytes += entry.control_encoded_bytes;
            position_bytes += entry.position_encoded_bytes;
            control_count += entry.control_element_count;
            position_count += entry.position_element_count;
        }
        const std::uint64_t payload_bytes =
            bioseq::parallelism::saturating_add(
                control_bytes, position_bytes);
        const std::uint64_t descriptor_bytes =
            bioseq::parallelism::saturating_mul(
                descriptors.size(),
                sizeof(bioseq::gpu::vulkan::
                    GpuShapeV5ResidentPartitionDescriptor));
        if (payload_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || descriptor_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || payload_bytes != manifest.payload_bytes
            || control_count != manifest.control_count
            || position_count != manifest.position_count
            || directory.encoded_bytes().size() != manifest.directory_bytes
            || manifest.directory_bytes
                != static_cast<std::uint64_t>(entries.size())
                    * MinimizerBucketDirEntry::format5_encoded_bytes) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::metadata_mismatch);
        }

        const std::uint64_t control_request =
            std::max<std::uint64_t>(control_bytes, 16u);
        const std::uint64_t position_request =
            std::max<std::uint64_t>(position_bytes, 16u);
        const std::uint64_t descriptor_request =
            std::max<std::uint64_t>(descriptor_bytes, 16u);
        if (control_request > runtime.features().max_storage_buffer_range
            || position_request > runtime.features().max_storage_buffer_range
            || descriptor_request > runtime.features().max_storage_buffer_range) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::arithmetic_overflow);
        }
        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const std::uint64_t control_admission =
            runtime.buffer_allocation_requirement(control_request, usage);
        const std::uint64_t position_admission =
            runtime.buffer_allocation_requirement(position_request, usage);
        const std::uint64_t descriptor_admission =
            runtime.buffer_allocation_requirement(descriptor_request, usage);
        const std::uint64_t admitted_bytes =
            bioseq::parallelism::saturating_add(
                control_admission,
                bioseq::parallelism::saturating_add(
                    position_admission, descriptor_admission));
        if (admitted_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::arithmetic_overflow);
        }
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(admitted_bytes);
        if (!permit) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::admission);
        }
        auto control_buffer = runtime.create_device_buffer(
            control_request, usage);
        auto position_buffer = runtime.create_device_buffer(
            position_request, usage);
        auto descriptor_buffer = runtime.create_device_buffer(
            descriptor_request, usage);
        const std::uint64_t allocation_bytes =
            bioseq::parallelism::saturating_add(
                control_buffer.allocation_bytes,
                bioseq::parallelism::saturating_add(
                    position_buffer.allocation_bytes,
                    descriptor_buffer.allocation_bytes));
        if (allocation_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || allocation_bytes > admitted_bytes) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::admission);
        }
        if (control_buffer.mapped == nullptr
            || position_buffer.mapped == nullptr
            || descriptor_buffer.mapped == nullptr) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::non_mappable_device_memory);
        }
        permit->shrink_to(allocation_bytes);
        std::memset(control_buffer.mapped, 0,
            static_cast<std::size_t>(control_buffer.bytes));
        std::memset(position_buffer.mapped, 0,
            static_cast<std::size_t>(position_buffer.bytes));
        std::memset(descriptor_buffer.mapped, 0,
            static_cast<std::size_t>(descriptor_buffer.bytes));
        if (descriptor_bytes != 0u) {
            std::memcpy(descriptor_buffer.mapped, descriptors.data(),
                static_cast<std::size_t>(descriptor_bytes));
        }

        const GpuShapeV5ResidentIdentity identity{
            .wire_format = manifest.wire_format,
            .generation_id = manifest.generation_id,
            .source_identity = manifest.source_identity,
            .source_content_checksum = manifest.source_content_checksum,
            .compatibility_checksum = head.compatibility_checksum,
            .manifest_checksum = head.manifest_checksum,
            .directory_checksum = manifest.directory_checksum,
            .payload_checksum = manifest.payload_checksum,
            .cold_load_identity = cold_load_identity,
            .directory_bytes = manifest.directory_bytes,
            .control_bytes = control_bytes,
            .position_bytes = position_bytes,
            .descriptor_bytes = descriptor_bytes,
            .control_count = control_count,
            .position_count = position_count,
            .descriptor_count = static_cast<std::uint32_t>(entries.size()),
            .device_uuid = runtime.features().device_uuid,
            .gpu_abi_version = manifest.gpu_abi_version,
        };
        auto resident = std::make_shared<GpuResidentShapeV5Target>(
            GpuResidentShapeV5Target::ConstructionToken{},
            std::move(*permit), std::move(control_buffer),
            std::move(position_buffer), std::move(descriptor_buffer),
            identity, manifest.directory_bytes, control_bytes,
            position_bytes, descriptor_bytes, control_count,
            position_count, static_cast<std::uint32_t>(entries.size()));
        resident->proof_.descriptor_bytes_copied = descriptor_bytes;

        std::vector<ExtentPlan> plans;
        std::vector<IndexSubKeyRequest> requests;
        plans.reserve(entries.size() * 2u);
        requests.reserve(entries.size() * 2u);
        const auto append_plan = [entries, &manifest, &control_offsets,
                                  &position_offsets](
                                     std::size_t ordinal, bool position,
                                     auto& plan_out, auto& request_out)
                -> bool {
            const MinimizerBucketDirEntry& entry = entries[ordinal];
            const auto position_sub_key =
                derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) return false;
            const IndexSubKeyRequest request{
                .generation_id = manifest.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = position ? *position_sub_key : entry.sub_key,
                .first_chunk_no = position
                    ? entry.position_first_chunk : entry.control_first_chunk,
                .expected_chunk_count = position
                    ? entry.position_chunk_count : entry.control_chunk_count,
                .expected_total_bytes = position
                    ? entry.position_encoded_bytes : entry.control_encoded_bytes,
                .expected_checksum = position
                    ? entry.position_checksum : entry.control_checksum,
            };
            plan_out.push_back(ExtentPlan{
                .request = request,
                .destination_offset = position
                    ? position_offsets[ordinal] : control_offsets[ordinal],
                .position = position,
            });
            request_out.push_back(request);
            return true;
        };
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            if (!append_plan(ordinal, false, plans, requests)) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::partition_mismatch);
            }
        }
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            if (!append_plan(ordinal, true, plans, requests)) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::partition_mismatch);
            }
        }
        std::uint64_t expected_payload_rows = 0u;
        for (const IndexSubKeyRequest& request : requests) {
            if (expected_payload_rows
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - request.expected_chunk_count) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::arithmetic_overflow);
            }
            expected_payload_rows += request.expected_chunk_count;
        }
        return GpuShapeV5FullGenerationIngest{
            std::move(resident), std::move(plans), std::move(requests),
            manifest.payload_checksum, expected_payload_rows};
    }

    [[nodiscard]] std::span<const IndexSubKeyRequest> requests() const noexcept {
        return requests_;
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_validated_row(
        const ValidatedMinimizerGenerationRowReceipt& receipt) noexcept
    {
        std::expected<void, GpuPersistedIndexFoundationError> accepted{};
        const bool visited = visit_validated_minimizer_generation_row(
            receipt,
            [this, &accepted](
                const IndexChunkAddress& address,
                std::span<const std::byte> bytes,
                std::uint64_t row_checksum) noexcept {
                accepted = accept_chunk_impl(
                    address, bytes, row_checksum,
                    InputAuthority::validated_receipt);
            });
        if (!visited) {
            return fail<void>(
                GpuPersistedIndexFoundationError::unauthorized_extent);
        }
        return accepted;
    }

    /// Standalone corruption-test path. Production full-generation loading
    /// must pass ValidatedMinimizerGenerationRowReceipt instead.
    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_raw_chunk_for_test(
        const IndexChunkAddress& address,
        std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return fail<void>(GpuPersistedIndexFoundationError::byte_count);
        }
        const std::uint64_t row_checksum = crc64_index_chunk_row(
            address.generation_id, address.position_segment,
            address.sub_key, address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()), bytes);
        return accept_chunk_impl(
            address, bytes, row_checksum, InputAuthority::raw_rehashed);
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_raw_chunk_for_test(
        const IndexChunkAddress& address,
        const char* payload,
        std::size_t payload_bytes) noexcept
    {
        if (payload == nullptr && payload_bytes != 0u) {
            return fail<void>(GpuPersistedIndexFoundationError::byte_count);
        }
        return accept_raw_chunk_for_test(address, std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(payload), payload_bytes});
    }

    [[nodiscard]] std::expected<
        std::shared_ptr<const GpuResidentShapeV5Target>,
        GpuPersistedIndexFoundationError>
    finish()
    {
        if (failure_) return std::unexpected(*failure_);
        if (!resident_ || extent_cursor_ != plans_.size()
            || chunk_cursor_ != 0u) {
            return fail<std::shared_ptr<const GpuResidentShapeV5Target>>(
                GpuPersistedIndexFoundationError::byte_count);
        }
        // Only the receipt path is publishable. It proves that PostgreSQL's
        // single scan already checked each row while the aggregate below binds
        // the exact ordered checksum sequence to this manifest.
        if (input_authority_ != InputAuthority::validated_receipt
            || resident_->proof_.validated_receipt_rows
                != expected_payload_rows_
            || resident_->proof_.raw_rehashed_rows != 0u
            || resident_->proof_.payload_hash_byte_visits != 0u) {
            return fail<std::shared_ptr<const GpuResidentShapeV5Target>>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        if (payload_row_crc_.checksum() != payload_checksum_expected_) {
            return fail<std::shared_ptr<const GpuResidentShapeV5Target>>(
                GpuPersistedIndexFoundationError::checksum);
        }
        resident_->proof_.cold_cache_misses = 1u;
        resident_->proof_.full_generations_ingested = 1u;
        resident_->proof_.device_allocation_bytes =
            resident_->controls_.allocation_bytes
            + resident_->positions_.allocation_bytes
            + resident_->descriptors_.allocation_bytes;
        resident_->proof_.admitted_device_bytes = resident_->permit_.bytes();
        if (!resident_->validated()) {
            return fail<std::shared_ptr<const GpuResidentShapeV5Target>>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        std::shared_ptr<const GpuResidentShapeV5Target> published = resident_;
        resident_.reset();
        return published;
    }

private:
    enum class InputAuthority : std::uint8_t {
        unset,
        validated_receipt,
        raw_rehashed,
    };

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_chunk_impl(
        const IndexChunkAddress& address,
        std::span<const std::byte> bytes,
        std::uint64_t admitted_row_checksum,
        InputAuthority authority) noexcept
    {
        if (failure_) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::poisoned_stream);
        }
        if (input_authority_ == InputAuthority::unset) {
            input_authority_ = authority;
        } else if (input_authority_ != authority) {
            return fail<void>(
                GpuPersistedIndexFoundationError::unauthorized_extent);
        }
        if (!resident_ || extent_cursor_ >= plans_.size()) {
            return fail<void>(GpuPersistedIndexFoundationError::byte_count);
        }
        const ExtentPlan& plan = plans_[extent_cursor_];
        const IndexSubKeyRequest& request = plan.request;
        if (chunk_cursor_ >= request.expected_chunk_count
            || address.generation_id != request.generation_id
            || address.position_segment != request.position_segment
            || address.sub_key != request.sub_key
            || address.chunk_no != request.first_chunk_no + chunk_cursor_) {
            return fail<void>(
                GpuPersistedIndexFoundationError::unauthorized_extent);
        }
        const std::uint64_t quantum = kMinimizerFormat5ExtentPayloadBytes;
        const std::uint64_t chunk_offset =
            static_cast<std::uint64_t>(chunk_cursor_) * quantum;
        if (chunk_offset >= request.expected_total_bytes) {
            return fail<void>(GpuPersistedIndexFoundationError::byte_count);
        }
        const std::uint64_t expected_bytes = std::min<std::uint64_t>(
            quantum, request.expected_total_bytes - chunk_offset);
        bioseq::gpu::vulkan::Buffer& destination = plan.position
            ? resident_->positions_ : resident_->controls_;
        const std::uint64_t destination_offset =
            bioseq::parallelism::saturating_add(
                plan.destination_offset, chunk_offset);
        if (bytes.size() != expected_bytes
            || destination_offset
                == (std::numeric_limits<std::uint64_t>::max)()
            || bytes.size() > destination.bytes
            || destination_offset
                > destination.bytes - bytes.size()) {
            return fail<void>(GpuPersistedIndexFoundationError::byte_count);
        }
        std::memcpy(
            static_cast<std::byte*>(destination.mapped)
                + destination_offset,
            bytes.data(), bytes.size());
        if (authority == InputAuthority::raw_rehashed) {
            extent_crc_.update(bytes);
            resident_->proof_.raw_rehashed_rows++;
            resident_->proof_.payload_hash_byte_visits += bytes.size() * 2u;
        } else {
            resident_->proof_.validated_receipt_rows++;
        }
        payload_row_crc_.update_little_endian(admitted_row_checksum);
        if (plan.position) {
            resident_->proof_.position_bytes_copied += bytes.size();
        } else {
            resident_->proof_.control_bytes_copied += bytes.size();
        }
        ++chunk_cursor_;
        if (chunk_cursor_ == request.expected_chunk_count) {
            if (authority == InputAuthority::raw_rehashed
                && extent_crc_.checksum() != request.expected_checksum) {
                return fail<void>(GpuPersistedIndexFoundationError::checksum);
            }
            if (plan.position) {
                ++resident_->proof_.position_extents_validated;
            } else {
                ++resident_->proof_.control_extents_validated;
            }
            ++extent_cursor_;
            chunk_cursor_ = 0u;
            extent_crc_ = {};
        }
        return {};
    }
    struct ExtentPlan {
        IndexSubKeyRequest request{};
        std::uint64_t destination_offset = 0u;
        bool position = false;
    };

    GpuShapeV5FullGenerationIngest(
        std::shared_ptr<GpuResidentShapeV5Target> resident,
        std::vector<ExtentPlan> plans,
        std::vector<IndexSubKeyRequest> requests,
        std::uint64_t payload_checksum_expected,
        std::uint64_t expected_payload_rows) noexcept
        : resident_(std::move(resident)), plans_(std::move(plans)),
          requests_(std::move(requests)),
          payload_checksum_expected_(payload_checksum_expected),
          expected_payload_rows_(expected_payload_rows) {}

    template <class T>
    [[nodiscard]] std::expected<T, GpuPersistedIndexFoundationError>
    fail(GpuPersistedIndexFoundationError error) noexcept {
        failure_ = error;
        return std::unexpected(error);
    }

    std::shared_ptr<GpuResidentShapeV5Target> resident_{};
    std::vector<ExtentPlan> plans_{};
    std::vector<IndexSubKeyRequest> requests_{};
    std::size_t extent_cursor_ = 0u;
    std::uint32_t chunk_cursor_ = 0u;
    Crc64Ecma182 extent_crc_{};
    Crc64Ecma182 payload_row_crc_{};
    std::uint64_t payload_checksum_expected_ = 0u;
    std::uint64_t expected_payload_rows_ = 0u;
    InputAuthority input_authority_ = InputAuthority::unset;
    std::optional<GpuPersistedIndexFoundationError> failure_{};
};

/// Phase of the bounded format-4 persisted stream.  This is intentionally a
/// protocol state, not an execution-mode proof: runtime kernels do not yet
/// produce the completion receipts consumed below.
enum class GpuGrouped32StreamPhase : std::uint8_t {
    controls,
    count_finalize,
    position_admission,
    positions,
    target_seal,
    sealed,
    failed,
};

struct GpuGrouped32StreamConfig {
    std::uint32_t ring_slot_count = 2u;
    std::uint64_t slot_payload_capacity_bytes =
        kMinimizerExtentPayloadBytes;
    std::uint32_t slot_partition_capacity = 8u;
    std::uint64_t query_identity = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t max_occ = 0u;
    const bioseq::gpu::vulkan::Buffer* device_query_records = nullptr;
    const bioseq::gpu::vulkan::Buffer* device_query_minimizers = nullptr;
    std::uint32_t query_record_count = 0u;
    std::uint32_t query_minimizer_count = 0u;
};

/// Immutable identity bound into every future runtime completion.  It contains
/// no host query hashes and cannot be used to reconstruct them.
struct GpuGrouped32StreamIdentity {
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_checksum = 0u;
    std::uint64_t compatibility_checksum = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t directory_checksum = 0u;
    std::uint64_t directory_bytes = 0u;
    std::uint64_t query_identity = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t max_occ = 0u;
    GpuDeviceUuid device_uuid{};
    std::uint32_t gpu_abi_version = 0u;

    [[nodiscard]] bool operator==(
        const GpuGrouped32StreamIdentity&) const = default;
};

struct GpuGrouped32StreamSlotHandle {
    std::uint32_t slot_ordinal = 0u;
    std::uint32_t group_ordinal = 0u;
    std::uint64_t allocation_generation = 0u;
    bool position_phase = false;

    [[nodiscard]] bool operator==(
        const GpuGrouped32StreamSlotHandle&) const = default;
};

/// View handed to a future runtime dispatch.  The payload is the exact
/// persisted LE byte sequence; descriptors are format-4 ABI2 records.  No host
/// payload decode or global-position conversion occurs while constructing it.
struct GpuGrouped32StreamDispatchView {
    GpuGrouped32StreamSlotHandle handle{};
    const bioseq::gpu::vulkan::Buffer* payload = nullptr;
    const bioseq::gpu::vulkan::Buffer* descriptors = nullptr;
    std::uint64_t payload_bytes = 0u;
    std::uint32_t descriptor_count = 0u;
    std::uint64_t receipt_identity = 0u;
};

/// Typed receipt placeholders.  They are structurally strict, but public test
/// construction is not cryptographic evidence that Vulkan ran.  Consequently
/// the terminal object below is permanently ineligible for all-GPU acceptance.
struct GpuGrouped32ControlGroupCompletion {
    GpuGrouped32StreamSlotHandle handle{};
    GpuDeviceUuid device_uuid{};
    std::uint64_t receipt_identity = 0u;
    std::uint64_t device_match_state_identity = 0u;
    std::uint64_t matched_anchor_upper_bound = 0u;
    bool accepted = false;
};

struct GpuGrouped32CountFinalizeCompletion {
    GpuDeviceUuid device_uuid{};
    std::uint64_t receipt_identity = 0u;
    std::uint64_t device_control_state_identity = 0u;
    std::uint64_t authorized_anchor_count = 0u;
    std::uint64_t authorization_digest = 0u;
    std::vector<std::uint32_t> authorized_directory_ordinals{};
    bool accepted = false;
};

struct GpuGrouped32PositionCapacityBinding {
    GpuDeviceUuid device_uuid{};
    std::uint64_t authorization_digest = 0u;
    std::uint64_t anchor_capacity = 0u;
    std::uint64_t anchor_bytes = 0u;
    std::uint64_t target_buffer_identity = 0u;
    bool accepted = false;
};

struct GpuGrouped32PositionGroupCompletion {
    GpuGrouped32StreamSlotHandle handle{};
    GpuDeviceUuid device_uuid{};
    std::uint64_t receipt_identity = 0u;
    std::uint64_t target_buffer_identity = 0u;
    std::uint64_t emitted_anchor_count = 0u;
    bool accepted = false;
};

struct GpuGrouped32TargetSealCompletion {
    GpuDeviceUuid device_uuid{};
    std::uint64_t receipt_identity = 0u;
    std::uint64_t authorization_digest = 0u;
    std::uint64_t target_buffer_identity = 0u;
    std::uint64_t emitted_anchor_count = 0u;
    std::uint64_t anchor_digest = 0u;
    bool accepted = false;
};

struct GpuGrouped32PositionAuthorizationView {
    std::uint64_t authorized_anchor_count = 0u;
    std::uint64_t authorization_digest = 0u;
    std::span<const std::uint32_t> authorized_directory_ordinals{};
};

enum class GpuGrouped32ReceiptStage : std::uint8_t {
    control_group,
    count_finalize,
    position_group,
    target_seal,
};

struct GpuGrouped32ReceiptRecord {
    GpuGrouped32ReceiptStage stage =
        GpuGrouped32ReceiptStage::control_group;
    std::uint32_t group_ordinal = 0u;
    std::uint64_t receipt_identity = 0u;
    std::uint64_t device_state_identity = 0u;
    std::uint64_t item_count = 0u;
};

/// Move-only terminal foundation record.  It proves only that the host-side
/// streaming protocol was followed.  No accepted factory can turn it into a
/// production all-GPU execution proof before the real runtime operations land.
class GpuGrouped32SealedTargetStream {
public:
    GpuGrouped32SealedTargetStream(
        const GpuGrouped32SealedTargetStream&) = delete;
    GpuGrouped32SealedTargetStream& operator=(
        const GpuGrouped32SealedTargetStream&) = delete;
    GpuGrouped32SealedTargetStream(
        GpuGrouped32SealedTargetStream&&) noexcept = default;
    GpuGrouped32SealedTargetStream& operator=(
        GpuGrouped32SealedTargetStream&&) noexcept = default;

    [[nodiscard]] const GpuGrouped32StreamIdentity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const GpuPersistedIndexLocalProof& local_proof() const noexcept {
        return proof_;
    }
    [[nodiscard]] std::span<const GpuGrouped32ReceiptRecord>
    ordered_receipts() const noexcept { return receipts_; }
    [[nodiscard]] std::span<const std::uint32_t>
    authorized_directory_ordinals() const noexcept {
        return authorized_ordinals_;
    }
    [[nodiscard]] std::uint64_t authorized_anchor_count() const noexcept {
        return authorized_anchor_count_;
    }
    [[nodiscard]] std::uint64_t authorization_digest() const noexcept {
        return authorization_digest_;
    }
    [[nodiscard]] std::uint64_t target_buffer_identity() const noexcept {
        return target_buffer_identity_;
    }
    [[nodiscard]] std::uint64_t anchor_digest() const noexcept {
        return anchor_digest_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::Buffer*
    runtime_anchor_buffer() const noexcept {
        return runtime_state_ ? &runtime_state_->anchors() : nullptr;
    }
    [[nodiscard]] bool foundation_validated() const noexcept {
        if (identity_.gpu_abi_version != kMinimizerGrouped32GpuAbiVersion
            || identity_.generation_id == 0u
            || identity_.query_identity == 0u
            || target_buffer_identity_ == 0u
            || authorization_digest_ == 0u
            || (authorized_anchor_count_ != 0u && anchor_digest_ == 0u)
            || proof_.saturated || proof_.has_forbidden_work()
            || proof_.grouped32_target_seals != 1u
            || proof_.grouped32_authorized_anchors
                != authorized_anchor_count_
            || proof_.grouped32_authorized_partitions
                != authorized_ordinals_.size()
            || receipts_.size() < 2u) {
            return false;
        }
        std::unordered_set<std::uint64_t> receipt_identities;
        std::size_t cursor = 0u;
        std::uint64_t control_upper_bound = 0u;
        std::uint64_t control_state_identity = 0u;
        std::uint32_t expected_control_group = 0u;
        while (cursor < receipts_.size()
            && receipts_[cursor].stage
                == GpuGrouped32ReceiptStage::control_group) {
            const auto& receipt = receipts_[cursor];
            if (receipt.group_ordinal != expected_control_group++
                || receipt.receipt_identity == 0u
                || receipt.device_state_identity == 0u
                || !receipt_identities.insert(receipt.receipt_identity).second) {
                return false;
            }
            if (control_state_identity == 0u) {
                control_state_identity = receipt.device_state_identity;
            } else if (receipt.device_state_identity
                    != control_state_identity) {
                return false;
            }
            const std::uint64_t next = bioseq::parallelism::saturating_add(
                control_upper_bound, receipt.item_count);
            if (next == (std::numeric_limits<std::uint64_t>::max)()
                && receipt.item_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - control_upper_bound) {
                return false;
            }
            control_upper_bound = next;
            ++cursor;
        }
        if (cursor >= receipts_.size()
            || receipts_[cursor].stage
                != GpuGrouped32ReceiptStage::count_finalize) {
            return false;
        }
        const auto& authorization = receipts_[cursor++];
        if (authorization.group_ordinal != 0u
            || authorization.receipt_identity == 0u
            || authorization.device_state_identity == 0u
            || (control_state_identity != 0u
                && authorization.device_state_identity
                    != control_state_identity)
            || authorization.item_count != authorized_anchor_count_
            || authorized_anchor_count_ > control_upper_bound
            || !receipt_identities.insert(
                authorization.receipt_identity).second) {
            return false;
        }
        std::uint64_t emitted_sum = 0u;
        std::uint32_t expected_position_group = 0u;
        while (cursor < receipts_.size()
            && receipts_[cursor].stage
                == GpuGrouped32ReceiptStage::position_group) {
            const auto& receipt = receipts_[cursor];
            if (receipt.group_ordinal != expected_position_group++
                || receipt.receipt_identity == 0u
                || receipt.device_state_identity != target_buffer_identity_
                || !receipt_identities.insert(receipt.receipt_identity).second) {
                return false;
            }
            const std::uint64_t next = bioseq::parallelism::saturating_add(
                emitted_sum, receipt.item_count);
            if (next == (std::numeric_limits<std::uint64_t>::max)()
                && receipt.item_count
                    > (std::numeric_limits<std::uint64_t>::max)() - emitted_sum) {
                return false;
            }
            emitted_sum = next;
            ++cursor;
        }
        if (cursor + 1u != receipts_.size()
            || receipts_[cursor].stage != GpuGrouped32ReceiptStage::target_seal
            || receipts_[cursor].group_ordinal != 0u
            || receipts_[cursor].receipt_identity == 0u
            || receipts_[cursor].device_state_identity
                != target_buffer_identity_
            || receipts_[cursor].item_count != authorized_anchor_count_
            || emitted_sum != authorized_anchor_count_
            || !receipt_identities.insert(
                receipts_[cursor].receipt_identity).second
            || proof_.grouped32_control_groups_completed
                != expected_control_group
            || proof_.grouped32_position_groups_completed
                != expected_position_group) {
            return false;
        }
        if (runtime_operation_receipts_complete_) {
            const std::uint64_t required_anchor_bytes =
                bioseq::parallelism::saturating_mul(
                    authorized_anchor_count_,
                    sizeof(bioseq::gpu::vulkan::GpuGrouped32GlobalAnchorWire));
            if (!runtime_state_ || !runtime_state_->anchors().valid()
                || runtime_state_->control_state_identity()
                    != authorization.device_state_identity
                || runtime_state_->target_buffer_identity()
                    != target_buffer_identity_
                || runtime_state_->authorization_digest()
                    != authorization_digest_
                || runtime_state_->authorized_anchor_count()
                    != authorized_anchor_count_
                || required_anchor_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || runtime_state_->anchors().bytes
                    < std::max<std::uint64_t>(required_anchor_bytes,
                        sizeof(bioseq::gpu::vulkan::GpuGrouped32GlobalAnchorWire))) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] static constexpr bool
    qualifies_as_all_gpu_execution() noexcept { return false; }
    [[nodiscard]] bool runtime_operation_receipts_complete() const noexcept {
        return runtime_operation_receipts_complete_;
    }

private:
    GpuGrouped32SealedTargetStream(
        GpuGrouped32StreamIdentity identity,
        GpuPersistedIndexLocalProof proof,
        std::vector<GpuGrouped32ReceiptRecord> receipts,
        std::vector<std::uint32_t> authorized_ordinals,
        std::uint64_t authorized_anchor_count,
        std::uint64_t authorization_digest,
        std::uint64_t target_buffer_identity,
        std::uint64_t anchor_digest,
        bool runtime_operation_receipts_complete,
        std::optional<bioseq::gpu::vulkan::GpuGrouped32RuntimeStreamState>
            runtime_state) noexcept
        : identity_(identity), proof_(proof), receipts_(std::move(receipts)),
          authorized_ordinals_(std::move(authorized_ordinals)),
          authorized_anchor_count_(authorized_anchor_count),
          authorization_digest_(authorization_digest),
          target_buffer_identity_(target_buffer_identity),
          anchor_digest_(anchor_digest),
          runtime_operation_receipts_complete_(
              runtime_operation_receipts_complete),
          runtime_state_(std::move(runtime_state)) {}

    GpuGrouped32StreamIdentity identity_{};
    GpuPersistedIndexLocalProof proof_{};
    std::vector<GpuGrouped32ReceiptRecord> receipts_{};
    std::vector<std::uint32_t> authorized_ordinals_{};
    std::uint64_t authorized_anchor_count_ = 0u;
    std::uint64_t authorization_digest_ = 0u;
    std::uint64_t target_buffer_identity_ = 0u;
    std::uint64_t anchor_digest_ = 0u;
    bool runtime_operation_receipts_complete_ = false;
    std::optional<bioseq::gpu::vulkan::GpuGrouped32RuntimeStreamState>
        runtime_state_{};

    friend class GpuGrouped32TwoPhaseStream;
};

/// Bounded, two-phase, raw format-4 stream.  It owns S reusable payload slots,
/// validates exact qualified chunk order/length/CRC, and does not retain a
/// whole selected index.  Controls must finish and authorize exact directory
/// ordinals before any position request can be observed.
class GpuGrouped32TwoPhaseStream {
public:
    GpuGrouped32TwoPhaseStream(const GpuGrouped32TwoPhaseStream&) = delete;
    GpuGrouped32TwoPhaseStream& operator=(
        const GpuGrouped32TwoPhaseStream&) = delete;
    GpuGrouped32TwoPhaseStream(
        GpuGrouped32TwoPhaseStream&&) noexcept = default;
    GpuGrouped32TwoPhaseStream& operator=(
        GpuGrouped32TwoPhaseStream&&) = delete;

    [[nodiscard]] static std::expected<
        GpuGrouped32TwoPhaseStream,
        GpuPersistedIndexFoundationError>
    begin(
        bioseq::gpu::vulkan::Runtime& runtime,
        const MinimizerGenerationHead& head,
        const MinimizerGenerationManifest& manifest,
        const ExpectedMinimizerCompatibility& expected,
        const DirView& authoritative_directory,
        std::shared_ptr<const GpuResidentAuthoritativeDirectory>
            resident_directory,
        const GpuOpaqueDirectoryOrdinals& selection,
        const GpuGrouped32StreamConfig& config)
    {
        if constexpr (std::endian::native != std::endian::little) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::unsupported_endianness);
        }
        if (!runtime.usable()
            || !gpu_device_uuid_is_bound(runtime.features().device_uuid)) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::invalid_identity);
        }
        const bool format4 =
            manifest.wire_format
                == MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32
            && authoritative_directory.profile().wire_format
                == MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32
            && manifest.gpu_abi_version == kMinimizerGrouped32GpuAbiVersion
            && authoritative_directory.profile().gpu_abi_version
                == kMinimizerGrouped32GpuAbiVersion;
        if (!format4
            || !validate_minimizer_generation_compatibility(
                manifest, expected)
            || !validate_minimizer_generation_metadata(
                head, manifest, authoritative_directory)) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::metadata_mismatch);
        }
        if (!resident_directory) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::invalid_identity);
        }
        const auto selected = selection.values_for(*resident_directory);
        const GpuPersistedDirectoryIdentity& directory_identity =
            resident_directory->identity();
        const bool identity_matches =
            selected.has_value()
            && directory_identity.generation_id == manifest.generation_id
            && directory_identity.source_identity == manifest.source_identity
            && directory_identity.source_content_checksum
                == manifest.source_content_checksum
            && directory_identity.compatibility_checksum
                == head.compatibility_checksum
            && directory_identity.directory_checksum
                == manifest.directory_checksum
            && directory_identity.directory_bytes
                == authoritative_directory.encoded_bytes().size()
            && directory_identity.directory_bytes == manifest.directory_bytes
            && directory_identity.entry_count
                == authoritative_directory.entries().size()
            && directory_identity.position_segment_count
                == manifest.position_segment_count
            && directory_identity.device_uuid
                == runtime.features().device_uuid
            && directory_identity.gpu_abi_version
                == kMinimizerGrouped32GpuAbiVersion;
        if (!identity_matches) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::selection_identity_mismatch);
        }
        if (config.ring_slot_count < 2u || config.ring_slot_count > 4u
            || config.slot_payload_capacity_bytes == 0u
            || config.slot_payload_capacity_bytes
                % kMinimizerGrouped32ControlEncodedBytes != 0u
            || config.slot_payload_capacity_bytes
                % kMinimizerGrouped32PositionEncodedBytes != 0u
            || config.slot_partition_capacity == 0u
            || config.query_identity == 0u || config.max_occ == 0u
            || config.max_occ
                > (std::numeric_limits<std::uint32_t>::max)()
            || config.device_query_records == nullptr
            || config.device_query_minimizers == nullptr
            || config.query_record_count == 0u
            || config.query_minimizer_count == 0u
            || config.unique_hash_count != config.query_minimizer_count
            || config.slot_payload_capacity_bytes
                > runtime.features().max_storage_buffer_range
            || static_cast<std::uint64_t>(config.slot_partition_capacity)
                    * sizeof(bioseq::gpu::vulkan::GpuGrouped32PartitionWire)
                > runtime.features().max_storage_buffer_range) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::invalid_stream_config);
        }
        if (!std::is_sorted(selected->begin(), selected->end())
            || std::adjacent_find(selected->begin(), selected->end())
                != selected->end()
            || (!selected->empty()
                && selected->back()
                    >= authoritative_directory.entries().size())
            || (config.unique_hash_count == 0u && !selected->empty())) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::malformed_ordinal_result);
        }

        GpuGrouped32TwoPhaseStream stream;
        stream.runtime_ = &runtime;
        stream.resident_directory_ = std::move(resident_directory);
        stream.config_ = config;
        stream.manifest_ = manifest;
        stream.identity_ = GpuGrouped32StreamIdentity{
            .generation_id = manifest.generation_id,
            .source_identity = manifest.source_identity,
            .source_content_checksum = manifest.source_content_checksum,
            .compatibility_checksum = head.compatibility_checksum,
            .manifest_checksum = head.manifest_checksum,
            .directory_checksum = manifest.directory_checksum,
            .directory_bytes = manifest.directory_bytes,
            .query_identity = config.query_identity,
            .unique_hash_count = config.unique_hash_count,
            .max_occ = config.max_occ,
            .device_uuid = runtime.features().device_uuid,
            .gpu_abi_version = kMinimizerGrouped32GpuAbiVersion,
        };
        stream.selected_ordinals_.assign(selected->begin(), selected->end());
        const auto planned = stream.build_groups(
            authoritative_directory, stream.selected_ordinals_, false);
        if (!planned) return std::unexpected(planned.error());
        stream.control_groups_ = std::move(*planned);
        stream.control_receipts_.resize(stream.control_groups_.size());
        stream.runtime_control_receipts_.assign(
            stream.control_groups_.size(), false);

        std::uint64_t selected_segment_count = 0u;
        std::optional<std::int32_t> previous_segment;
        for (const Group& group : stream.control_groups_) {
            if (!previous_segment || *previous_segment != group.position_segment) {
                ++selected_segment_count;
                previous_segment = group.position_segment;
            }
        }
        // Directory hash ranges do not overlap within one position segment, so
        // each query minimizer can produce at most one compact match per
        // selected segment.  Sizing by all persisted controls made cold loads
        // proportional to the target index rather than the query.
        const std::uint64_t match_capacity =
            bioseq::parallelism::saturating_mul(
                config.query_minimizer_count, selected_segment_count);
        if (match_capacity == (std::numeric_limits<std::uint64_t>::max)()
            || match_capacity
                > (std::numeric_limits<std::uint32_t>::max)()
            || stream.control_groups_.size()
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::arithmetic_overflow);
        }

        const std::uint64_t descriptor_capacity_bytes =
            static_cast<std::uint64_t>(config.slot_partition_capacity)
            * sizeof(bioseq::gpu::vulkan::GpuGrouped32PartitionWire);
        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const std::uint64_t payload_allocation_bytes =
            runtime.buffer_allocation_requirement(
                config.slot_payload_capacity_bytes, usage);
        const std::uint64_t descriptor_allocation_bytes =
            runtime.buffer_allocation_requirement(
                std::max<std::uint64_t>(descriptor_capacity_bytes, 16u), usage);
        const std::uint64_t per_slot_bytes =
            bioseq::parallelism::saturating_add(
                payload_allocation_bytes, descriptor_allocation_bytes);
        const std::uint64_t admitted_bytes =
            bioseq::parallelism::saturating_mul(
                per_slot_bytes, config.ring_slot_count);
        if (per_slot_bytes == (std::numeric_limits<std::uint64_t>::max)()
            || admitted_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::arithmetic_overflow);
        }
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(admitted_bytes);
        if (!permit) {
            return std::unexpected(GpuPersistedIndexFoundationError::admission);
        }
        stream.permit_ = std::move(*permit);
        stream.slots_.reserve(config.ring_slot_count);
        std::uint64_t allocation_bytes = 0u;
        for (std::uint32_t index = 0u;
             index < config.ring_slot_count; ++index) {
            Slot slot;
            slot.payload = runtime.create_device_buffer(
                config.slot_payload_capacity_bytes, usage);
            slot.descriptors = runtime.create_device_buffer(
                std::max<std::uint64_t>(descriptor_capacity_bytes, 16u),
                usage);
            if (slot.payload.allocation_bytes > payload_allocation_bytes
                || slot.descriptors.allocation_bytes
                    > descriptor_allocation_bytes) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::admission);
            }
            if (slot.payload.mapped == nullptr
                || slot.descriptors.mapped == nullptr) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::
                        non_mappable_device_memory);
            }
            allocation_bytes = bioseq::parallelism::saturating_add(
                allocation_bytes,
                bioseq::parallelism::saturating_add(
                    slot.payload.allocation_bytes,
                    slot.descriptors.allocation_bytes));
            if (allocation_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::arithmetic_overflow);
            }
            stream.slots_.push_back(std::move(slot));
        }
        if (allocation_bytes > stream.permit_.bytes()) {
            return std::unexpected(GpuPersistedIndexFoundationError::admission);
        }
        stream.permit_.shrink_to(allocation_bytes);
        try {
            stream.runtime_state_.emplace(
                runtime.begin_grouped32_runtime_stream(
                    bioseq::gpu::vulkan::GpuGrouped32RuntimeStateInputs{
                        .device_query_records = config.device_query_records,
                        .device_query_minimizers =
                            config.device_query_minimizers,
                        .query_record_count = config.query_record_count,
                        .query_minimizer_count = config.query_minimizer_count,
                        .occurrence_count = config.query_minimizer_count,
                        .match_capacity = static_cast<std::uint32_t>(
                            std::max<std::uint64_t>(match_capacity, 1u)),
                        .directory_entry_count = static_cast<std::uint32_t>(
                            authoritative_directory.entries().size()),
                        .expected_control_groups = static_cast<std::uint32_t>(
                            stream.control_groups_.size()),
                        .max_occ = static_cast<std::uint32_t>(config.max_occ),
                        .kmer_size = manifest.kmer_size,
                        .query_identity = config.query_identity,
                    }));
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError&) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::admission);
        }
        stream.phase_ = stream.control_groups_.empty()
            ? GpuGrouped32StreamPhase::count_finalize
            : GpuGrouped32StreamPhase::controls;
        return stream;
    }

    [[nodiscard]] GpuGrouped32StreamPhase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] const GpuGrouped32StreamIdentity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const GpuPersistedIndexLocalProof& local_proof() const noexcept {
        return proof_;
    }
    [[nodiscard]] std::size_t control_group_count() const noexcept {
        return control_groups_.size();
    }
    [[nodiscard]] std::size_t position_group_count() const noexcept {
        return position_groups_.size();
    }
    [[nodiscard]] std::uint64_t admitted_slot_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] std::expected<
        std::vector<IndexSubKeyRequest>, GpuPersistedIndexFoundationError>
    control_requests(std::uint32_t group) const {
        return requests_for(control_groups_, group);
    }

    [[nodiscard]] std::expected<
        std::vector<IndexSubKeyRequest>, GpuPersistedIndexFoundationError>
    position_requests(std::uint32_t group) const {
        if (phase_ != GpuGrouped32StreamPhase::positions
            && phase_ != GpuGrouped32StreamPhase::target_seal) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::unauthorized_extent);
        }
        return requests_for(position_groups_, group);
    }

    [[nodiscard]] std::expected<
        GpuGrouped32StreamSlotHandle, GpuPersistedIndexFoundationError>
    open_control_group(std::uint32_t group) {
        return open_group(group, false);
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_control_chunk(
        const GpuGrouped32StreamSlotHandle& handle,
        const IndexChunkAddress& address,
        std::span<const std::byte> bytes) noexcept {
        return accept_chunk(handle, address, bytes, false);
    }

    [[nodiscard]] std::expected<
        GpuGrouped32StreamDispatchView, GpuPersistedIndexFoundationError>
    close_control_group(const GpuGrouped32StreamSlotHandle& handle) noexcept {
        return close_group(handle, false);
    }

    /// Execute stage 44 on the exact closed slot.  Only this method can mark a
    /// control receipt as runtime-produced; the public completion seam remains
    /// useful for protocol-only tests but can never qualify the runtime receipt
    /// predicate.
    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    dispatch_control_group(
        const GpuGrouped32StreamDispatchView& dispatch) noexcept {
        if (phase_ != GpuGrouped32StreamPhase::controls
            || runtime_ == nullptr || !runtime_state_
            || dispatch.handle.position_phase
            || dispatch.payload == nullptr || dispatch.descriptors == nullptr
            || dispatch.payload_bytes
                % kMinimizerGrouped32ControlEncodedBytes != 0u) {
            return poison<void>(
                GpuPersistedIndexFoundationError::invalid_stream_phase);
        }
        try {
            const auto receipt = runtime_->run_grouped32_control_validate_count(
                *runtime_state_,
                bioseq::gpu::vulkan::GpuGrouped32RawGroupInputs{
                    .descriptors = dispatch.descriptors,
                    .payload = dispatch.payload,
                    .descriptor_count = dispatch.descriptor_count,
                    .payload_element_count = static_cast<std::uint32_t>(
                        dispatch.payload_bytes
                        / kMinimizerGrouped32ControlEncodedBytes),
                    .group_ordinal = dispatch.handle.group_ordinal,
                    .receipt_identity = dispatch.receipt_identity,
                });
            const auto completed = complete_control_group(
                GpuGrouped32ControlGroupCompletion{
                    .handle = dispatch.handle,
                    .device_uuid = receipt.device_uuid,
                    .receipt_identity = receipt.receipt_identity,
                    .device_match_state_identity =
                        receipt.control_state_identity,
                    .matched_anchor_upper_bound =
                        receipt.matched_anchor_upper_bound,
                    .accepted = receipt.accepted,
                });
            if (!completed) return completed;
            runtime_control_receipts_[dispatch.handle.group_ordinal] = true;
            return {};
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError&) {
            return poison<void>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    complete_control_group(
        const GpuGrouped32ControlGroupCompletion& completion) noexcept {
        if (phase_ != GpuGrouped32StreamPhase::controls) {
            return poison<void>(
                GpuPersistedIndexFoundationError::invalid_stream_phase);
        }
        const auto slot_result = checked_slot(completion.handle, false);
        if (!slot_result) return poison<void>(slot_result.error());
        Slot& slot = **slot_result;
        Group& group = control_groups_[completion.handle.group_ordinal];
        if (!slot.closed || group.completed
            || completion.device_uuid != identity_.device_uuid
            || completion.receipt_identity != slot.receipt_identity
            || completion.device_match_state_identity == 0u
            || !completion.accepted) {
            return poison<void>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        control_receipts_[completion.handle.group_ordinal] =
            GpuGrouped32ReceiptRecord{
                .stage = GpuGrouped32ReceiptStage::control_group,
                .group_ordinal = completion.handle.group_ordinal,
                .receipt_identity = completion.receipt_identity,
                .device_state_identity =
                    completion.device_match_state_identity,
                .item_count = completion.matched_anchor_upper_bound,
            };
        group.completed = true;
        release_slot(slot, group);
        add_gpu_persisted_proof_counter(
            proof_, proof_.grouped32_control_groups_completed, 1u);
        if (all_groups_completed(control_groups_)) {
            phase_ = GpuGrouped32StreamPhase::count_finalize;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t count_finalize_receipt_identity() const noexcept {
        return phase_ == GpuGrouped32StreamPhase::count_finalize
            ? phase_receipt_identity(0x434F554E54554C4Cull) : 0u;
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    complete_count_finalize(
        GpuGrouped32CountFinalizeCompletion completion) {
        if (phase_ != GpuGrouped32StreamPhase::count_finalize
            || completion.device_uuid != identity_.device_uuid
            || completion.receipt_identity
                != count_finalize_receipt_identity()
            || completion.device_control_state_identity == 0u
            || completion.authorization_digest == 0u
            || !completion.accepted
            || !std::is_sorted(
                completion.authorized_directory_ordinals.begin(),
                completion.authorized_directory_ordinals.end())
            || std::adjacent_find(
                completion.authorized_directory_ordinals.begin(),
                completion.authorized_directory_ordinals.end())
                != completion.authorized_directory_ordinals.end()
            || (completion.authorized_anchor_count == 0u)
                != completion.authorized_directory_ordinals.empty()
            || !std::ranges::includes(
                selected_ordinals_,
                completion.authorized_directory_ordinals)) {
            return poison<void>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        authorized_anchor_count_ = completion.authorized_anchor_count;
        authorization_digest_ = completion.authorization_digest;
        authorized_ordinals_ =
            std::move(completion.authorized_directory_ordinals);
        count_finalize_receipt_ = GpuGrouped32ReceiptRecord{
            .stage = GpuGrouped32ReceiptStage::count_finalize,
            .group_ordinal = 0u,
            .receipt_identity = completion.receipt_identity,
            .device_state_identity = completion.device_control_state_identity,
            .item_count = completion.authorized_anchor_count,
        };
        add_gpu_persisted_proof_counter(
            proof_, proof_.grouped32_authorized_partitions,
            authorized_ordinals_.size());
        add_gpu_persisted_proof_counter(
            proof_, proof_.grouped32_authorized_anchors,
            authorized_anchor_count_);
        phase_ = GpuGrouped32StreamPhase::position_admission;
        return {};
    }

    /// Execute target-global stage 45, consume its opaque ordinal/capacity
    /// result, allocate the exact target anchor buffer in the runtime, then
    /// admit only the authorized raw-position groups.
    [[nodiscard]] std::expected<
        GpuGrouped32PositionAuthorizationView,
        GpuPersistedIndexFoundationError>
    dispatch_count_finalize_and_bind_positions(
        const DirView& authoritative_directory) {
        if (phase_ != GpuGrouped32StreamPhase::count_finalize
            || runtime_ == nullptr || !runtime_state_
            || !std::ranges::all_of(runtime_control_receipts_,
                [](bool value) noexcept { return value; })) {
            return poison<GpuGrouped32PositionAuthorizationView>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        try {
            const auto receipt =
                runtime_->run_grouped32_count_finalize_authorize(
                    *runtime_state_, count_finalize_receipt_identity());
            const auto completed = complete_count_finalize(
                GpuGrouped32CountFinalizeCompletion{
                    .device_uuid = receipt.device_uuid,
                    .receipt_identity = receipt.receipt_identity,
                    .device_control_state_identity =
                        receipt.control_state_identity,
                    .authorized_anchor_count =
                        receipt.authorized_anchor_count,
                    .authorization_digest = receipt.authorization_digest,
                    .authorized_directory_ordinals =
                        receipt.authorized_directory_ordinals,
                    .accepted = receipt.accepted,
                });
            if (!completed) {
                return std::unexpected(completed.error());
            }
            runtime_authorization_receipt_ = true;
            const auto bound = bind_position_capacity(
                authoritative_directory,
                GpuGrouped32PositionCapacityBinding{
                    .device_uuid = receipt.device_uuid,
                    .authorization_digest = receipt.authorization_digest,
                    .anchor_capacity = receipt.authorized_anchor_count,
                    .anchor_bytes = receipt.anchor_bytes,
                    .target_buffer_identity = receipt.target_buffer_identity,
                    .accepted = receipt.accepted,
                });
            if (!bound) return std::unexpected(bound.error());
            runtime_position_receipts_.assign(
                position_groups_.size(), false);
            runtime_->bind_grouped32_expected_position_groups(
                *runtime_state_, static_cast<std::uint32_t>(
                    position_groups_.size()));
            return position_authorization();
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError&) {
            return poison<GpuGrouped32PositionAuthorizationView>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
    }

    [[nodiscard]] std::expected<
        GpuGrouped32PositionAuthorizationView,
        GpuPersistedIndexFoundationError>
    position_authorization() const noexcept {
        if (phase_ != GpuGrouped32StreamPhase::position_admission
            && phase_ != GpuGrouped32StreamPhase::positions
            && phase_ != GpuGrouped32StreamPhase::target_seal) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::unauthorized_extent);
        }
        return GpuGrouped32PositionAuthorizationView{
            .authorized_anchor_count = authorized_anchor_count_,
            .authorization_digest = authorization_digest_,
            .authorized_directory_ordinals = authorized_ordinals_,
        };
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    bind_position_capacity(
        const DirView& authoritative_directory,
        const GpuGrouped32PositionCapacityBinding& binding) {
        const std::uint64_t expected_bytes =
            bioseq::parallelism::saturating_mul(
                authorized_anchor_count_,
                sizeof(bioseq::gpu::vulkan::GpuGrouped32GlobalAnchorWire));
        if (phase_ != GpuGrouped32StreamPhase::position_admission
            || expected_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || binding.device_uuid != identity_.device_uuid
            || binding.authorization_digest != authorization_digest_
            || binding.anchor_capacity != authorized_anchor_count_
            || binding.anchor_bytes != expected_bytes
            || binding.target_buffer_identity == 0u || !binding.accepted) {
            return poison<void>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        const auto planned = build_groups(
            authoritative_directory, authorized_ordinals_, true);
        if (!planned) return poison<void>(planned.error());
        position_groups_ = std::move(*planned);
        position_receipts_.resize(position_groups_.size());
        target_buffer_identity_ = binding.target_buffer_identity;
        phase_ = position_groups_.empty()
            ? GpuGrouped32StreamPhase::target_seal
            : GpuGrouped32StreamPhase::positions;
        return {};
    }

    [[nodiscard]] std::expected<
        GpuGrouped32StreamSlotHandle, GpuPersistedIndexFoundationError>
    open_position_group(std::uint32_t group) {
        return open_group(group, true);
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_position_chunk(
        const GpuGrouped32StreamSlotHandle& handle,
        const IndexChunkAddress& address,
        std::span<const std::byte> bytes) noexcept {
        return accept_chunk(handle, address, bytes, true);
    }

    [[nodiscard]] std::expected<
        GpuGrouped32StreamDispatchView, GpuPersistedIndexFoundationError>
    close_position_group(const GpuGrouped32StreamSlotHandle& handle) noexcept {
        return close_group(handle, true);
    }

    /// Execute stage 46 directly over the exact raw local32 slot and the
    /// target-global authorization state produced by stage 45.
    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    dispatch_position_group(
        const GpuGrouped32StreamDispatchView& dispatch) noexcept {
        if (phase_ != GpuGrouped32StreamPhase::positions
            || runtime_ == nullptr || !runtime_state_
            || !dispatch.handle.position_phase
            || dispatch.payload == nullptr || dispatch.descriptors == nullptr
            || dispatch.payload_bytes
                % kMinimizerGrouped32PositionEncodedBytes != 0u) {
            return poison<void>(
                GpuPersistedIndexFoundationError::invalid_stream_phase);
        }
        try {
            const auto receipt = runtime_->run_grouped32_position_validate_emit(
                *runtime_state_,
                bioseq::gpu::vulkan::GpuGrouped32RawGroupInputs{
                    .descriptors = dispatch.descriptors,
                    .payload = dispatch.payload,
                    .descriptor_count = dispatch.descriptor_count,
                    .payload_element_count = static_cast<std::uint32_t>(
                        dispatch.payload_bytes
                        / kMinimizerGrouped32PositionEncodedBytes),
                    .group_ordinal = dispatch.handle.group_ordinal,
                    .receipt_identity = dispatch.receipt_identity,
                });
            const auto completed = complete_position_group(
                GpuGrouped32PositionGroupCompletion{
                    .handle = dispatch.handle,
                    .device_uuid = receipt.device_uuid,
                    .receipt_identity = receipt.receipt_identity,
                    .target_buffer_identity =
                        receipt.target_buffer_identity,
                    .emitted_anchor_count = receipt.emitted_anchor_count,
                    .accepted = receipt.accepted,
                });
            if (!completed) return completed;
            runtime_position_receipts_[dispatch.handle.group_ordinal] = true;
            return {};
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError&) {
            return poison<void>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    complete_position_group(
        const GpuGrouped32PositionGroupCompletion& completion) noexcept {
        if (phase_ != GpuGrouped32StreamPhase::positions) {
            return poison<void>(
                GpuPersistedIndexFoundationError::invalid_stream_phase);
        }
        const auto slot_result = checked_slot(completion.handle, true);
        if (!slot_result) return poison<void>(slot_result.error());
        Slot& slot = **slot_result;
        Group& group = position_groups_[completion.handle.group_ordinal];
        if (!slot.closed || group.completed
            || completion.device_uuid != identity_.device_uuid
            || completion.receipt_identity != slot.receipt_identity
            || completion.target_buffer_identity != target_buffer_identity_
            || !completion.accepted) {
            return poison<void>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        position_receipts_[completion.handle.group_ordinal] =
            GpuGrouped32ReceiptRecord{
                .stage = GpuGrouped32ReceiptStage::position_group,
                .group_ordinal = completion.handle.group_ordinal,
                .receipt_identity = completion.receipt_identity,
                .device_state_identity = completion.target_buffer_identity,
                .item_count = completion.emitted_anchor_count,
            };
        group.completed = true;
        release_slot(slot, group);
        add_gpu_persisted_proof_counter(
            proof_, proof_.grouped32_position_groups_completed, 1u);
        if (all_groups_completed(position_groups_)) {
            phase_ = GpuGrouped32StreamPhase::target_seal;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t target_seal_receipt_identity() const noexcept {
        return phase_ == GpuGrouped32StreamPhase::target_seal
            ? phase_receipt_identity(0x5345414C54415247ull) : 0u;
    }

    /// Execute stage 47 and consume the private operation-bound receipt.  The
    /// returned target stream still cannot by itself qualify the complete
    /// all-GPU algorithm; it only proves all four persisted target-stream
    /// operations ran on the selected Vulkan device.
    [[nodiscard]] std::expected<
        GpuGrouped32SealedTargetStream,
        GpuPersistedIndexFoundationError>
    dispatch_target_seal() {
        if (phase_ != GpuGrouped32StreamPhase::target_seal
            || runtime_ == nullptr || !runtime_state_
            || !runtime_authorization_receipt_
            || !std::ranges::all_of(runtime_position_receipts_,
                [](bool value) noexcept { return value; })) {
            return poison<GpuGrouped32SealedTargetStream>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        try {
            const auto receipt = runtime_->run_grouped32_target_seal(
                *runtime_state_, target_seal_receipt_identity());
            runtime_seal_receipt_pending_ = receipt.accepted;
            return complete_target_seal(
                GpuGrouped32TargetSealCompletion{
                    .device_uuid = receipt.device_uuid,
                    .receipt_identity = receipt.receipt_identity,
                    .authorization_digest = receipt.authorization_digest,
                    .target_buffer_identity = receipt.target_buffer_identity,
                    .emitted_anchor_count = receipt.emitted_anchor_count,
                    .anchor_digest = receipt.anchor_digest,
                    .accepted = receipt.accepted,
                });
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError&) {
            return poison<GpuGrouped32SealedTargetStream>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
    }

    [[nodiscard]] std::expected<
        GpuGrouped32SealedTargetStream,
        GpuPersistedIndexFoundationError>
    complete_target_seal(
        const GpuGrouped32TargetSealCompletion& completion) {
        if (phase_ != GpuGrouped32StreamPhase::target_seal
            || completion.device_uuid != identity_.device_uuid
            || completion.receipt_identity
                != target_seal_receipt_identity()
            || completion.authorization_digest != authorization_digest_
            || completion.target_buffer_identity != target_buffer_identity_
            || completion.emitted_anchor_count != authorized_anchor_count_
            || (authorized_anchor_count_ != 0u
                && completion.anchor_digest == 0u)
            || !completion.accepted || proof_.has_forbidden_work()
            || proof_.saturated) {
            return poison<GpuGrouped32SealedTargetStream>(
                GpuPersistedIndexFoundationError::completion_mismatch);
        }
        add_gpu_persisted_proof_counter(
            proof_, proof_.grouped32_target_seals, 1u);
        const bool runtime_operation_receipts_complete =
            std::exchange(runtime_seal_receipt_pending_, false)
            && runtime_authorization_receipt_
            && std::ranges::all_of(runtime_control_receipts_,
                [](bool value) noexcept { return value; })
            && std::ranges::all_of(runtime_position_receipts_,
                [](bool value) noexcept { return value; });
        std::vector<GpuGrouped32ReceiptRecord> receipts;
        receipts.reserve(
            control_receipts_.size() + position_receipts_.size() + 2u);
        receipts.insert(
            receipts.end(), control_receipts_.begin(), control_receipts_.end());
        receipts.push_back(count_finalize_receipt_);
        receipts.insert(
            receipts.end(), position_receipts_.begin(), position_receipts_.end());
        receipts.push_back(GpuGrouped32ReceiptRecord{
            .stage = GpuGrouped32ReceiptStage::target_seal,
            .group_ordinal = 0u,
            .receipt_identity = completion.receipt_identity,
            .device_state_identity = completion.target_buffer_identity,
            .item_count = completion.emitted_anchor_count,
        });
        phase_ = GpuGrouped32StreamPhase::sealed;
        std::optional<bioseq::gpu::vulkan::GpuGrouped32RuntimeStreamState>
            retained_runtime_state;
        if (runtime_operation_receipts_complete) {
            retained_runtime_state = std::move(runtime_state_);
        }
        return GpuGrouped32SealedTargetStream{
            identity_, proof_, std::move(receipts), authorized_ordinals_,
            authorized_anchor_count_, authorization_digest_,
            target_buffer_identity_, completion.anchor_digest,
            runtime_operation_receipts_complete,
            std::move(retained_runtime_state)};
    }

private:
    struct ExtentPlan {
        std::uint32_t directory_ordinal = 0u;
        MinimizerBucketDirEntry entry{};
        IndexSubKeyRequest request{};
        std::uint64_t payload_offset = 0u;
        std::uint64_t segment_base_bp = 0u;
        std::uint64_t segment_end_bp = 0u;
    };

    struct Group {
        std::int32_t position_segment = 0;
        std::vector<ExtentPlan> extents{};
        std::uint64_t payload_bytes = 0u;
        bool open = false;
        bool completed = false;
    };

    struct Slot {
        bioseq::gpu::vulkan::Buffer payload{};
        bioseq::gpu::vulkan::Buffer descriptors{};
        std::uint64_t allocation_generation = 0u;
        std::uint64_t receipt_identity = 0u;
        std::uint64_t bytes_received = 0u;
        std::uint32_t group_ordinal = 0u;
        std::size_t extent_cursor = 0u;
        std::uint32_t chunk_cursor = 0u;
        Crc64Ecma182 extent_crc{};
        bool busy = false;
        bool position_phase = false;
        bool closed = false;
    };

    GpuGrouped32TwoPhaseStream() = default;

    [[nodiscard]] static bool all_groups_completed(
        const std::vector<Group>& groups) noexcept {
        return std::all_of(groups.begin(), groups.end(),
            [](const Group& group) noexcept { return group.completed; });
    }

    [[nodiscard]] std::expected<
        std::vector<Group>, GpuPersistedIndexFoundationError>
    build_groups(
        const DirView& directory,
        std::span<const std::uint32_t> ordinals,
        bool positions) const {
        if (directory.profile().wire_format
                != MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32
            || directory.profile().gpu_abi_version
                != kMinimizerGrouped32GpuAbiVersion
            || crc64_ecma182(directory.encoded_bytes())
                != identity_.directory_checksum
            || directory.encoded_bytes().size() != identity_.directory_bytes) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::metadata_mismatch);
        }
        std::vector<Group> groups;
        for (const std::uint32_t ordinal : ordinals) {
            const MinimizerBucketDirEntry* entry =
                directory.entry_by_ordinal(ordinal);
            if (entry == nullptr
                || entry->partition_id != ordinal
                || validate_minimizer_bucket_dir_entry(
                    *entry, directory.profile()).has_value()) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::partition_mismatch);
            }
            const std::uint64_t extent_bytes = positions
                ? entry->position_encoded_bytes
                : entry->control_encoded_bytes;
            const std::uint32_t extent_chunks = positions
                ? entry->position_chunk_count : entry->control_chunk_count;
            const std::uint32_t first_chunk = positions
                ? entry->position_first_chunk : entry->control_first_chunk;
            const std::uint64_t checksum = positions
                ? entry->position_checksum : entry->control_checksum;
            const auto physical_sub_key = positions
                ? derive_minimizer_position_extent_sub_key(entry->sub_key)
                : std::expected<index_sub_key_t, MinimizerGenerationWireError>{
                    entry->sub_key};
            const auto core = minimizer_position_segment_core(
                manifest_, static_cast<std::uint32_t>(entry->position_segment));
            if (!physical_sub_key || !core || extent_bytes == 0u
                || extent_bytes > config_.slot_payload_capacity_bytes) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::invalid_stream_config);
            }
            const bool needs_group = groups.empty()
                || groups.back().position_segment != entry->position_segment
                || groups.back().extents.size()
                    >= config_.slot_partition_capacity
                || groups.back().payload_bytes
                    > config_.slot_payload_capacity_bytes - extent_bytes;
            if (needs_group) {
                groups.push_back(Group{
                    .position_segment = entry->position_segment});
            }
            Group& group = groups.back();
            const std::uint64_t payload_offset = group.payload_bytes;
            group.extents.push_back(ExtentPlan{
                .directory_ordinal = ordinal,
                .entry = *entry,
                .request = IndexSubKeyRequest{
                    .generation_id = identity_.generation_id,
                    .position_segment = entry->position_segment,
                    .sub_key = *physical_sub_key,
                    .first_chunk_no = first_chunk,
                    .expected_chunk_count = extent_chunks,
                    .expected_total_bytes = extent_bytes,
                    .expected_checksum = checksum,
                },
                .payload_offset = payload_offset,
                .segment_base_bp = core->begin_bp,
                .segment_end_bp = core->end_bp,
            });
            group.payload_bytes += extent_bytes;
        }
        return groups;
    }

    [[nodiscard]] static std::expected<
        std::vector<IndexSubKeyRequest>, GpuPersistedIndexFoundationError>
    requests_for(const std::vector<Group>& groups, std::uint32_t group) {
        if (group >= groups.size()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::partition_mismatch);
        }
        std::vector<IndexSubKeyRequest> requests;
        requests.reserve(groups[group].extents.size());
        for (const ExtentPlan& extent : groups[group].extents) {
            requests.push_back(extent.request);
        }
        return requests;
    }

    [[nodiscard]] std::uint64_t phase_receipt_identity(
        std::uint64_t tag) const noexcept {
        std::uint64_t digest = splitmix64(identity_.generation_id ^ tag);
        digest ^= splitmix64(identity_.query_identity + (digest << 1u));
        digest ^= splitmix64(
            identity_.directory_checksum + (digest << 1u));
        return digest == 0u ? 1u : digest;
    }

    [[nodiscard]] std::uint64_t group_receipt_identity(
        std::uint32_t group,
        std::uint64_t allocation_generation,
        bool positions) const noexcept {
        std::uint64_t digest = phase_receipt_identity(
            positions ? 0x504F534954494F4Eull : 0x434F4E54524F4C53ull);
        digest ^= splitmix64(
            static_cast<std::uint64_t>(group) + (digest << 1u));
        digest ^= splitmix64(allocation_generation + (digest << 1u));
        return digest == 0u ? 1u : digest;
    }

    [[nodiscard]] std::expected<
        GpuGrouped32StreamSlotHandle, GpuPersistedIndexFoundationError>
    open_group(std::uint32_t group_ordinal, bool positions) {
        const GpuGrouped32StreamPhase expected_phase = positions
            ? GpuGrouped32StreamPhase::positions
            : GpuGrouped32StreamPhase::controls;
        auto& groups = positions ? position_groups_ : control_groups_;
        if (phase_ != expected_phase || group_ordinal >= groups.size()
            || groups[group_ordinal].completed
            || groups[group_ordinal].open) {
            return poison<GpuGrouped32StreamSlotHandle>(
                GpuPersistedIndexFoundationError::invalid_stream_phase);
        }
        const std::uint32_t slot_ordinal =
            group_ordinal % config_.ring_slot_count;
        Slot& slot = slots_[slot_ordinal];
        if (slot.busy) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::slot_busy);
        }
        if (slot.allocation_generation
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return poison<GpuGrouped32StreamSlotHandle>(
                GpuPersistedIndexFoundationError::arithmetic_overflow);
        }
        ++slot.allocation_generation;
        if (slot.allocation_generation > 1u) {
            add_gpu_persisted_proof_counter(
                proof_, proof_.grouped32_slot_reuses, 1u);
        }
        slot.receipt_identity = group_receipt_identity(
            group_ordinal, slot.allocation_generation, positions);
        slot.bytes_received = 0u;
        slot.group_ordinal = group_ordinal;
        slot.extent_cursor = 0u;
        slot.chunk_cursor = 0u;
        slot.extent_crc = Crc64Ecma182{};
        slot.busy = true;
        slot.position_phase = positions;
        slot.closed = false;
        Group& group = groups[group_ordinal];
        group.open = true;

        std::memset(slot.descriptors.mapped, 0,
            static_cast<std::size_t>(slot.descriptors.bytes));
        auto* descriptors = static_cast<
            bioseq::gpu::vulkan::GpuGrouped32PartitionWire*>(
                slot.descriptors.mapped);
        for (std::size_t index = 0u; index < group.extents.size(); ++index) {
            const ExtentPlan& extent = group.extents[index];
            descriptors[index] =
                bioseq::gpu::vulkan::GpuGrouped32PartitionWire{
                    .partition_id = extent.entry.partition_id,
                    .directory_ordinal = extent.directory_ordinal,
                    .position_segment = static_cast<std::uint32_t>(
                        extent.entry.position_segment),
                    .control_offset = positions ? 0u
                        : static_cast<std::uint32_t>(
                            extent.payload_offset
                            / kMinimizerGrouped32ControlEncodedBytes),
                    .control_count = static_cast<std::uint32_t>(
                        extent.entry.control_element_count),
                    .position_offset = positions
                        ? static_cast<std::uint32_t>(
                            extent.payload_offset
                            / kMinimizerGrouped32PositionEncodedBytes)
                        : 0u,
                    .position_count = static_cast<std::uint32_t>(
                        extent.entry.position_element_count),
                    .low_hash = static_cast<std::uint32_t>(
                        extent.entry.lo_hash),
                    .high_hash = static_cast<std::uint32_t>(
                        extent.entry.hi_hash),
                    .flags = positions ? 2u : 1u,
                    .segment_base_bp = extent.segment_base_bp,
                    .segment_end_bp = extent.segment_end_bp,
                };
        }
        ++inflight_slots_;
        proof_.grouped32_max_inflight_slots = std::max(
            proof_.grouped32_max_inflight_slots, inflight_slots_);
        add_gpu_persisted_proof_counter(
            proof_, positions
                ? proof_.grouped32_position_groups_opened
                : proof_.grouped32_control_groups_opened,
            1u);
        return GpuGrouped32StreamSlotHandle{
            .slot_ordinal = slot_ordinal,
            .group_ordinal = group_ordinal,
            .allocation_generation = slot.allocation_generation,
            .position_phase = positions,
        };
    }

    [[nodiscard]] std::expected<Slot*, GpuPersistedIndexFoundationError>
    checked_slot(
        const GpuGrouped32StreamSlotHandle& handle,
        bool positions) noexcept {
        auto& groups = positions ? position_groups_ : control_groups_;
        if (handle.slot_ordinal >= slots_.size()
            || handle.group_ordinal >= groups.size()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::stale_slot_handle);
        }
        Slot& slot = slots_[handle.slot_ordinal];
        if (!slot.busy || slot.position_phase != positions
            || handle.position_phase != positions
            || slot.group_ordinal != handle.group_ordinal
            || slot.allocation_generation != handle.allocation_generation
            || handle.slot_ordinal
                != handle.group_ordinal % config_.ring_slot_count) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::stale_slot_handle);
        }
        return &slot;
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_chunk(
        const GpuGrouped32StreamSlotHandle& handle,
        const IndexChunkAddress& address,
        std::span<const std::byte> bytes,
        bool positions) noexcept {
        const GpuGrouped32StreamPhase expected_phase = positions
            ? GpuGrouped32StreamPhase::positions
            : GpuGrouped32StreamPhase::controls;
        if (phase_ != expected_phase) {
            return poison<void>(
                GpuPersistedIndexFoundationError::invalid_stream_phase);
        }
        const auto slot_result = checked_slot(handle, positions);
        if (!slot_result) return poison<void>(slot_result.error());
        Slot& slot = **slot_result;
        Group& group = (positions ? position_groups_ : control_groups_)
            [handle.group_ordinal];
        if (slot.closed || slot.extent_cursor >= group.extents.size()) {
            return poison<void>(
                GpuPersistedIndexFoundationError::byte_count);
        }
        const ExtentPlan& extent = group.extents[slot.extent_cursor];
        const IndexSubKeyRequest& request = extent.request;
        if (slot.chunk_cursor >= request.expected_chunk_count
            || address.generation_id != request.generation_id
            || address.position_segment != request.position_segment
            || address.sub_key != request.sub_key
            || address.chunk_no
                != request.first_chunk_no + slot.chunk_cursor) {
            return poison<void>(
                positions
                    ? GpuPersistedIndexFoundationError::unauthorized_extent
                    : GpuPersistedIndexFoundationError::partition_mismatch);
        }
        const std::uint64_t chunk_offset =
            static_cast<std::uint64_t>(slot.chunk_cursor)
            * kMinimizerExtentPayloadBytes;
        const std::uint64_t expected_bytes = std::min<std::uint64_t>(
            kMinimizerExtentPayloadBytes,
            request.expected_total_bytes - chunk_offset);
        if (bytes.size() != expected_bytes
            || extent.payload_offset + chunk_offset
                    > config_.slot_payload_capacity_bytes - bytes.size()) {
            return poison<void>(
                GpuPersistedIndexFoundationError::byte_count);
        }
        std::memcpy(
            static_cast<std::byte*>(slot.payload.mapped)
                + extent.payload_offset + chunk_offset,
            bytes.data(), bytes.size());
        slot.extent_crc.update(bytes);
        slot.bytes_received += bytes.size();
        ++slot.chunk_cursor;
        if (slot.chunk_cursor == request.expected_chunk_count) {
            if (slot.extent_crc.checksum() != request.expected_checksum) {
                return poison<void>(
                    GpuPersistedIndexFoundationError::checksum);
            }
            add_gpu_persisted_proof_counter(
                proof_, positions
                    ? proof_.grouped32_position_extents_validated
                    : proof_.grouped32_control_extents_validated,
                1u);
            ++slot.extent_cursor;
            slot.chunk_cursor = 0u;
            slot.extent_crc = Crc64Ecma182{};
        }
        add_gpu_persisted_proof_counter(
            proof_, positions
                ? proof_.grouped32_position_bytes_streamed
                : proof_.grouped32_control_bytes_streamed,
            bytes.size());
        return {};
    }

    [[nodiscard]] std::expected<
        GpuGrouped32StreamDispatchView, GpuPersistedIndexFoundationError>
    close_group(
        const GpuGrouped32StreamSlotHandle& handle,
        bool positions) noexcept {
        const GpuGrouped32StreamPhase expected_phase = positions
            ? GpuGrouped32StreamPhase::positions
            : GpuGrouped32StreamPhase::controls;
        if (phase_ != expected_phase) {
            return poison<GpuGrouped32StreamDispatchView>(
                GpuPersistedIndexFoundationError::invalid_stream_phase);
        }
        const auto slot_result = checked_slot(handle, positions);
        if (!slot_result) {
            return poison<GpuGrouped32StreamDispatchView>(slot_result.error());
        }
        Slot& slot = **slot_result;
        const Group& group = (positions ? position_groups_ : control_groups_)
            [handle.group_ordinal];
        if (slot.closed || slot.extent_cursor != group.extents.size()
            || slot.chunk_cursor != 0u
            || slot.bytes_received != group.payload_bytes) {
            return poison<GpuGrouped32StreamDispatchView>(
                GpuPersistedIndexFoundationError::byte_count);
        }
        slot.closed = true;
        return GpuGrouped32StreamDispatchView{
            .handle = handle,
            .payload = &slot.payload,
            .descriptors = &slot.descriptors,
            .payload_bytes = group.payload_bytes,
            .descriptor_count = static_cast<std::uint32_t>(
                group.extents.size()),
            .receipt_identity = slot.receipt_identity,
        };
    }

    void release_slot(Slot& slot, Group& group) noexcept {
        slot.busy = false;
        slot.closed = false;
        group.open = false;
        if (inflight_slots_ != 0u) --inflight_slots_;
    }

    template <class T>
    [[nodiscard]] std::expected<T, GpuPersistedIndexFoundationError>
    poison(GpuPersistedIndexFoundationError error) noexcept {
        phase_ = GpuGrouped32StreamPhase::failed;
        return std::unexpected(error);
    }

    // Declared before slots so reverse destruction releases Vulkan allocations
    // before their shared memory-admission reservation.
    bioseq::parallelism::MemoryPermit permit_{};
    std::vector<Slot> slots_{};
    std::optional<bioseq::gpu::vulkan::GpuGrouped32RuntimeStreamState>
        runtime_state_{};
    bioseq::gpu::vulkan::Runtime* runtime_ = nullptr;
    std::shared_ptr<const GpuResidentAuthoritativeDirectory>
        resident_directory_{};
    GpuGrouped32StreamConfig config_{};
    GpuGrouped32StreamIdentity identity_{};
    MinimizerGenerationManifest manifest_{};
    GpuPersistedIndexLocalProof proof_{};
    std::vector<std::uint32_t> selected_ordinals_{};
    std::vector<std::uint32_t> authorized_ordinals_{};
    std::vector<Group> control_groups_{};
    std::vector<Group> position_groups_{};
    std::vector<GpuGrouped32ReceiptRecord> control_receipts_{};
    std::vector<GpuGrouped32ReceiptRecord> position_receipts_{};
    std::vector<bool> runtime_control_receipts_{};
    std::vector<bool> runtime_position_receipts_{};
    bool runtime_authorization_receipt_ = false;
    bool runtime_seal_receipt_pending_ = false;
    GpuGrouped32ReceiptRecord count_finalize_receipt_{};
    std::uint64_t authorized_anchor_count_ = 0u;
    std::uint64_t authorization_digest_ = 0u;
    std::uint64_t target_buffer_identity_ = 0u;
    std::uint64_t inflight_slots_ = 0u;
    GpuGrouped32StreamPhase phase_ = GpuGrouped32StreamPhase::failed;
};

} // namespace bioseq::indexing
