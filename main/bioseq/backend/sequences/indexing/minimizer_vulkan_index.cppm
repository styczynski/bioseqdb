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
            && built.ownership.ungated_emit_dispatches == 1u;
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
    if (directory.profile().wire_format
            != MinimizerGenerationWireFormat::sorted_dense_global64) {
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
            && directory_identity.sorted_dense_abi_version
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

} // namespace bioseq::indexing
