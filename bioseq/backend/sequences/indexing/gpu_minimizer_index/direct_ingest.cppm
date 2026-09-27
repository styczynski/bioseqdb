/**
 * @file direct_ingest.cppm
 * @brief Direct open-addressed GPU minimizer-index ingestion.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <fmt/format.h>
#include <libassert/assert.hpp>
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

export module bioseq.indexing.gpu_minimizer_index:direct_ingest;

import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// Device-visible 16-byte bucket shared with the minimizer hash-map representation.
using gpu_minimizer_slot = minimizer_hashmap_fragment_index::encoded_bucket;
static_assert(sizeof(gpu_minimizer_slot) == 16);
static_assert(offsetof(gpu_minimizer_slot, hash) == 0);
static_assert(offsetof(gpu_minimizer_slot, pos_offset) == 8);
static_assert(offsetof(gpu_minimizer_slot, pos_count) == 12);

/// Owning host image used only by the raw-target upload path.
///
/// Persisted ingestion does not accept this type and publishes fully owned streamed residents.
struct gpu_minimizer_index_upload {
    std::vector<gpu_minimizer_slot> slots;
    std::vector<std::uint64_t> positions;
    std::uint64_t target_len = 0;
    std::uint64_t table_size = 0;
    std::uint32_t slot_mask = 0;
};

/// Signals that an index cannot be represented by the 32-bit device ABI.
struct gpu_index_too_large final : std::exception {
    /// Return a stable diagnostic string for the device ABI limit.
    [[nodiscard]] const char* what() const noexcept override {
        return "GPU minimizer index exceeds 32-bit device slot limits";
    }
};

/// Signals malformed source buckets or failure to fill the planned device table.
struct gpu_index_direct_fill_invariant final : std::exception {
    /// Return a stable diagnostic string for a direct-fill contract failure.
    [[nodiscard]] const char* what() const noexcept override {
        return "GPU minimizer index direct-fill invariant failed";
    }
};

/// Typed diagnostic for a source-direct CPU-built minimizer-index upload. Probe
/// callers may continue to use the nullable handle, while strict pipeline
/// callers pass an output record and preserve the exact predicate that made
/// the handle unavailable. This record is stats only. It does not permit a
/// CPU fallback.
enum class gpu_index_upload_decline_reason : std::uint8_t {
    none,
    device_abi_overflow,
    storage_buffer_range_exceeded,
    direct_fill_invariant_failed,
    resident_memory_admission_denied,
    upload_peak_memory_admission_denied,
    non_host_visible_device_memory,
};

/// Device-side lookup representation selected for a resident index.
enum class gpu_index_representation : std::uint8_t {
    open_addressed_70,
    source_direct_sorted_dense,
};

/// ABI revision of the active source-direct sorted-dense device layout.
inline constexpr std::uint32_t
    k_minimizer_sorted_dense_gpu_abi_revision = 1u;

/// Return the stable diagnostic name for a device representation.
[[nodiscard]] inline constexpr std::string_view gpu_index_representation_name(
        gpu_index_representation representation) noexcept {
    switch (representation) {
        case gpu_index_representation::open_addressed_70:
            return "open_addressed_70";
        case gpu_index_representation::source_direct_sorted_dense:
            return "source_direct_sorted_dense";
    }
    return "unknown";
}

/// Capacity, admission, and allocation diagnostics for a declined GPU upload.
struct gpu_index_upload_decline {
    gpu_index_upload_decline_reason reason = gpu_index_upload_decline_reason::none;
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
    gpu_index_representation representation =
        gpu_index_representation::open_addressed_70;
    bioseq::parallelism::memory_admission_failure admission{};

    /// Test whether a decline reason was recorded.
    [[nodiscard]] constexpr bool is_declined() const noexcept {
        return reason != gpu_index_upload_decline_reason::none;
    }

    /// Test whether no decline reason was recorded.
    [[nodiscard]] constexpr bool is_accepted() const noexcept {
        return !is_declined();
    }
};

/// Return the stable diagnostic name for an upload decline reason.
[[nodiscard]] inline constexpr std::string_view
gpu_index_upload_decline_reason_name(
    gpu_index_upload_decline_reason reason) noexcept
{
    switch (reason) {
        case gpu_index_upload_decline_reason::none:
            return "none";
        case gpu_index_upload_decline_reason::device_abi_overflow:
            return "device_abi_overflow";
        case gpu_index_upload_decline_reason::storage_buffer_range_exceeded:
            return "storage_buffer_range_exceeded";
        case gpu_index_upload_decline_reason::direct_fill_invariant_failed:
            return "direct_fill_invariant_failed";
        case gpu_index_upload_decline_reason::resident_memory_admission_denied:
            return "resident_memory_admission_denied";
        case gpu_index_upload_decline_reason::upload_peak_memory_admission_denied:
            return "upload_peak_memory_admission_denied";
        case gpu_index_upload_decline_reason::non_host_visible_device_memory:
            return "non_host_visible_device_memory";
    }
    return "unknown";
}

/// Format all available upload-decline and memory-admission diagnostics.
[[nodiscard]] inline std::string format_gpu_index_upload_decline(
    const gpu_index_upload_decline& decline)
{
    const std::string_view reason =
        gpu_index_upload_decline_reason_name(decline.reason);
    if (decline.reason
            == gpu_index_upload_decline_reason::resident_memory_admission_denied
        || decline.reason
            == gpu_index_upload_decline_reason::upload_peak_memory_admission_denied) {
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

/// Identity and shape key for raw-target or recomputed resident indexes.
///
/// Grouped persisted ingestion does not accept this pointer-shaped key. A pointer source identity
/// is confined to a source-direct batch whose underlying CPU index remains immutable.
struct gpu_minimizer_index_cache_key {
    std::uintptr_t source_id = 0;
    std::uint64_t caller_identity = 0;
    std::uint64_t target_len = 0;
    std::uint64_t table_size = 0;
    std::uint64_t position_count = 0;
    // Operation-unique direct-resident identity.  All fields stay zero on the
    // CPU-index upload path. The direct route never keys a resident by
    // an empty placeholder's address and never aliases two operations whose
    // source, minimizer ABI, representation, or Vulkan device differs.
    std::uint64_t operation_identity = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t window_size = 0;
    std::uint32_t minimizer_size = 0;
    std::uint32_t minimizer_alignment = 0;
    std::uint32_t minimizer_hash_bits = 0;
    std::uint32_t minimizer_position_bits = 0;
    std::uint32_t sorted_dense_abi_revision = 0;
    gpu_index_representation representation =
        gpu_index_representation::open_addressed_70;
    bioseq::gpu::vulkan::gpu_device_uuid device_uuid{};
    bool source_direct_sorted_dense = false;
    bool gate_enabled = false;
    std::uint32_t gate_word_count = 0;
    std::uint32_t gate_mask = 0;
    std::uint64_t gate_digest_lo = 0;
    std::uint64_t gate_digest_hi = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> gate_canonical_words{};

    /// Compare all representation, source, device, and canonical gate authority fields.
    [[nodiscard]] bool operator==(
        const gpu_minimizer_index_cache_key& other) const
    {
        if (source_id != other.source_id
            || caller_identity != other.caller_identity
            || target_len != other.target_len || table_size != other.table_size
            || position_count != other.position_count
            || operation_identity != other.operation_identity
            || alphabet_size != other.alphabet_size
            || kmer_size != other.kmer_size
            || window_size != other.window_size
            || minimizer_size != other.minimizer_size
            || minimizer_alignment != other.minimizer_alignment
            || minimizer_hash_bits != other.minimizer_hash_bits
            || minimizer_position_bits != other.minimizer_position_bits
            || sorted_dense_abi_revision != other.sorted_dense_abi_revision
            || representation != other.representation
            || device_uuid != other.device_uuid
            || source_direct_sorted_dense
                != other.source_direct_sorted_dense
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

/// Project gate authority from a cache key into the Vulkan runtime identity type.
[[nodiscard]] inline bioseq::gpu::vulkan::gpu_index_gate_identity
gpu_index_gate_identity_from_key(
    const gpu_minimizer_index_cache_key& key)
{
    return bioseq::gpu::vulkan::gpu_index_gate_identity{
        .enabled = key.gate_enabled,
        .word_count = key.gate_word_count,
        .mask = key.gate_mask,
        .digest_lo = key.gate_digest_lo,
        .digest_hi = key.gate_digest_hi,
        .canonical_words = key.gate_canonical_words,
    };
}

/// Mix one 64-bit key component for host-side open addressing and cache hashing.
[[nodiscard]] inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/// Hash functor covering all cache-key fields that participate in equality.
struct gpu_minimizer_index_cache_key_hash {
    /// Compute a platform-sized mixed hash for an exact cache key.
    [[nodiscard]] std::size_t operator()(const gpu_minimizer_index_cache_key& k) const noexcept {
        std::uint64_t h = splitmix64(static_cast<std::uint64_t>(k.source_id));
        h ^= splitmix64(k.caller_identity + (h << 1));
        h ^= splitmix64(k.target_len + 0x9e3779b97f4a7c15ULL);
        h ^= splitmix64(k.table_size + (h << 1));
        h ^= splitmix64(k.position_count + (h << 1));
        h ^= splitmix64(k.operation_identity + (h << 1));
        h ^= splitmix64(k.alphabet_size + (h << 1));
        h ^= splitmix64(k.kmer_size + (h << 1));
        h ^= splitmix64(k.window_size + (h << 1));
        h ^= splitmix64(k.minimizer_size + (h << 1));
        h ^= splitmix64(k.minimizer_alignment + (h << 1));
        h ^= splitmix64(k.minimizer_hash_bits + (h << 1));
        h ^= splitmix64(k.minimizer_position_bits + (h << 1));
        h ^= splitmix64(k.sorted_dense_abi_revision + (h << 1));
        h ^= splitmix64(
            static_cast<std::uint64_t>(k.representation) + (h << 1));
        for (std::uint8_t byte : k.device_uuid) {
            h ^= splitmix64(static_cast<std::uint64_t>(byte) + (h << 1));
        }
        h ^= splitmix64(
            static_cast<std::uint64_t>(k.source_direct_sorted_dense)
                + (h << 1));
        h ^= splitmix64(static_cast<std::uint64_t>(k.gate_enabled) + (h << 1));
        h ^= splitmix64(k.gate_word_count + (h << 1));
        h ^= splitmix64(k.gate_mask + (h << 1));
        h ^= splitmix64(k.gate_digest_lo + (h << 1));
        h ^= splitmix64(k.gate_digest_hi + (h << 1));
        return static_cast<std::size_t>(h);
    }
};

/// Build a wave-local cache key from an immutable in-memory CPU index address and shape.
[[nodiscard]] inline gpu_minimizer_index_cache_key make_gpu_index_cache_key_for_in_memory(
    const minimizer_hashmap_fragment_index& idx) noexcept
{
    return gpu_minimizer_index_cache_key{
        .source_id = reinterpret_cast<std::uintptr_t>(&idx),
        .caller_identity = 0u,
        .target_len = static_cast<std::uint64_t>(idx.target_len),
        .table_size = static_cast<std::uint64_t>(idx.table_size()),
        .position_count = static_cast<std::uint64_t>(idx.positions.size()),
    };
}

/// Build a stable cache key from a nonzero persisted assembly identity and index shape.
[[nodiscard]] inline gpu_minimizer_index_cache_key make_gpu_index_cache_key_for_assembly(
    const minimizer_hashmap_fragment_index& idx,
    std::uint64_t assembly_identity) noexcept
{
    DEBUG_ASSERT(assembly_identity != 0, "persistent GPU assembly key must be nonzero");
    return gpu_minimizer_index_cache_key{
        .source_id = 0u,
        .caller_identity = assembly_identity,
        .target_len = static_cast<std::uint64_t>(idx.target_len),
        .table_size = static_cast<std::uint64_t>(idx.table_size()),
        .position_count = static_cast<std::uint64_t>(idx.positions.size()),
    };
}

/// Caller-supplied identity metadata used to select and bind a cache-key route.
struct gpu_index_cache_identity {
    // Zero means a stable, non-mutated in-memory index in the current wave.
    // Nonzero means persisted assembly or segmented recompute identity.
    std::uint64_t assembly_identity = 0;
    // Optional explicit shape for paths that preinsert a GPU-resident index
    // without materialising the CPU table/positions vectors.
    std::uint64_t table_size = 0;
    std::uint64_t position_count = 0;
    std::uint64_t operation_identity = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t window_size = 0;
    std::uint32_t minimizer_size = 0;
    std::uint32_t minimizer_alignment = 0;
    std::uint32_t minimizer_hash_bits = 0;
    std::uint32_t minimizer_position_bits = 0;
    std::uint32_t sorted_dense_abi_revision = 0;
    gpu_index_representation representation =
        gpu_index_representation::open_addressed_70;
    bioseq::gpu::vulkan::gpu_device_uuid device_uuid{};
    bool source_direct_sorted_dense = false;
    bool canonical_empty = false;
    bool gate_enabled = false;
    std::uint32_t gate_word_count = 0;
    std::uint32_t gate_mask = 0;
    std::uint64_t gate_digest_lo = 0;
    std::uint64_t gate_digest_hi = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> gate_canonical_words{};
};

/// Build the cache key for in-memory, assembly, or operation-owned direct-resident input.
[[nodiscard]] inline gpu_minimizer_index_cache_key make_gpu_index_cache_key(
    const minimizer_hashmap_fragment_index& idx,
    gpu_index_cache_identity identity) noexcept
{
    // Attach operation, device, representation, and canonical gate authority to a base key.
    const auto bind_gate = [identity](gpu_minimizer_index_cache_key key) noexcept {
        key.operation_identity = identity.operation_identity;
        key.alphabet_size = identity.alphabet_size;
        key.kmer_size = identity.kmer_size;
        key.window_size = identity.window_size;
        key.minimizer_size = identity.minimizer_size;
        key.minimizer_alignment = identity.minimizer_alignment;
        key.minimizer_hash_bits = identity.minimizer_hash_bits;
        key.minimizer_position_bits = identity.minimizer_position_bits;
        key.sorted_dense_abi_revision = identity.sorted_dense_abi_revision;
        key.representation = identity.representation;
        key.device_uuid = identity.device_uuid;
        key.source_direct_sorted_dense = identity.source_direct_sorted_dense;
        key.gate_enabled = identity.gate_enabled;
        key.gate_word_count = identity.gate_word_count;
        key.gate_mask = identity.gate_mask;
        key.gate_digest_lo = identity.gate_digest_lo;
        key.gate_digest_hi = identity.gate_digest_hi;
        key.gate_canonical_words = identity.gate_canonical_words;
        return key;
    };
    if (identity.source_direct_sorted_dense) {
        return bind_gate(gpu_minimizer_index_cache_key{
            .source_id = 0u,
            .caller_identity = identity.operation_identity,
            .target_len = static_cast<std::uint64_t>(idx.target_len),
            .table_size = identity.table_size,
            .position_count = identity.position_count,
        });
    }
    if (identity.assembly_identity == 0) {
        return bind_gate(make_gpu_index_cache_key_for_in_memory(idx));
    }
    if (identity.table_size != 0 || identity.position_count != 0) {
        return bind_gate(gpu_minimizer_index_cache_key{
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

/// Evidence that accounts for the actual bytes owned by a resident GPU index.
enum class gpu_resident_accounting_authority : std::uint8_t {
    none,
    reflected_process_footprint,
    retained_exact_permit,
};

/// Shared owner and host attestation for one device-resident minimizer index.
///
/// The permit is destroyed after both Vulkan buffers, and the representation fields mirror the
/// lookup ABI consumed by the seed shader.
struct gpu_resident_minimizer_index {
    // Declared first so it is destroyed last, after both resident buffers.
    bioseq::parallelism::memory_permit permit;
    bioseq::gpu::vulkan::buffer slots;
    bioseq::gpu::vulkan::buffer positions;
    bioseq::gpu::vulkan::gpu_index_gate_identity gate_identity{};
    gpu_minimizer_index_cache_key key{};
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    // When nonzero, `slots` is a source-direct hash-sorted dense bucket array
    // that the seed lookup binary-searches. `slot_mask` is then 0. When zero,
    // `slots` is the open-addressed table and `slot_mask` is the power-of-two
    // mask. The explicit `representation` field is the host-side attestation.
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
    std::uint64_t unique_key_count = 0;
    std::uint64_t slot_capacity = 0;
    std::uint64_t staging_bytes = 0;
    std::uint32_t load_ppm = 0;
    std::uint32_t max_probe = 0;
    gpu_index_representation representation =
        gpu_index_representation::open_addressed_70;
    bioseq::parallelism::memory_materialization_result materialization{};
    bioseq::gpu::vulkan::gpu_index_build_stats ownership{};
    gpu_resident_accounting_authority accounting_authority =
        gpu_resident_accounting_authority::none;
    std::uint64_t source_stream_checksum = 0u;
    std::uint64_t mapped_stream_checksum = 0u;
    std::uint64_t complete_source_traversals = 0u;
    std::uint64_t complete_mapped_traversals = 0u;
};
static_assert(std::is_standard_layout_v<gpu_resident_minimizer_index>);
static_assert(offsetof(gpu_resident_minimizer_index, permit)
    < offsetof(gpu_resident_minimizer_index, slots));
static_assert(offsetof(gpu_resident_minimizer_index, permit)
    < offsetof(gpu_resident_minimizer_index, positions));

/// Plan a power-of-two open-address table at no more than 70 percent load.
/// @throws gpu_index_too_large When the capacity cannot fit the 32-bit device ABI.
[[nodiscard]] inline std::uint32_t gpu_slot_capacity(std::uint64_t distinct) {
    // Power-of-two open addressing at <= 70% load.  ceil(10*D/7) is
    // deliberately evaluated only after an overflow guard. P=93,308,896
    // therefore plans 133,298,423 wanted slots -> C=2^27.
    if (distinct > ((std::numeric_limits<std::uint64_t>::max)() - 6u) / 10u) {
        throw gpu_index_too_large{};
    }
    const std::uint64_t wanted = (distinct * 10u + 6u) / 7u;
    if (wanted > (std::uint64_t{1} << 31)) {
        throw gpu_index_too_large{};
    }
    return static_cast<std::uint32_t>(std::bit_ceil(std::max<std::uint64_t>(2, wanted)));
}

/// Express a table load as parts per million, saturated at one million.
[[nodiscard]] inline std::uint32_t gpu_index_load_ppm(
        std::uint64_t distinct,
        std::uint64_t capacity) noexcept {
    if (capacity == 0u) return 0u;
    const std::uint64_t scaled =
        bioseq::common::saturating_mul(distinct, 1'000'000u);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        scaled / capacity, 1'000'000u));
}

/// Test both planned Vulkan storage-buffer byte ranges against the device limit.
[[nodiscard]] inline bool gpu_index_storage_ranges_fit(
        std::uint64_t slot_buffer_bytes,
        std::uint64_t position_buffer_bytes,
        std::uint64_t max_storage_buffer_range) noexcept {
    return max_storage_buffer_range != 0u
        && slot_buffer_bytes <= max_storage_buffer_range
        && position_buffer_bytes <= max_storage_buffer_range;
}

/// Observed shape and maximum probe length from direct device-buffer filling.
struct gpu_index_direct_fill_stats {
    std::uint64_t unique_key_count = 0;
    std::uint64_t slot_capacity = 0;
    std::uint32_t load_ppm = 0;
    std::uint32_t max_probe = 0;
};

/// Logical and minimum Vulkan allocation sizes for one device representation.
struct gpu_index_materialization_plan {
    gpu_index_representation representation =
        gpu_index_representation::open_addressed_70;
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
/// `source_direct_sorted_dense` is deliberately unavailable for an empty index because a
/// zero `bucket_count` is the shader ABI discriminator for open addressing.
[[nodiscard]] inline gpu_index_materialization_plan
plan_gpu_index_materialization(
        std::uint64_t distinct,
        std::uint64_t position_count,
        gpu_index_representation representation) {
    if (representation == gpu_index_representation::source_direct_sorted_dense
        && (distinct == 0u
            || distinct > (std::numeric_limits<std::uint32_t>::max)())) {
        throw gpu_index_too_large{};
    }
    const std::uint64_t slot_capacity =
        representation == gpu_index_representation::source_direct_sorted_dense
            ? distinct
            : gpu_slot_capacity(distinct);
    const std::uint64_t slot_bytes =
        bioseq::common::saturating_mul(
            slot_capacity, sizeof(gpu_minimizer_slot));
    const std::uint64_t position_bytes =
        bioseq::common::saturating_mul(
            position_count, sizeof(std::uint64_t));
    const std::uint64_t slot_buffer_bytes =
        std::max<std::uint64_t>(slot_bytes, sizeof(gpu_minimizer_slot));
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
        .resident_bytes = bioseq::common::saturating_add(
            slot_buffer_bytes, position_buffer_bytes),
        .load_ppm = gpu_index_load_ppm(distinct, slot_capacity),
    };
}

/// Construct the source-direct open-address table in its final mapped
/// Vulkan allocation. The CPU index remains authoritative. This function
/// allocates nothing, creates no full host upload vectors, and fails closed on
/// malformed buckets, duplicate keys, or a full probe cycle.
[[nodiscard]] inline gpu_index_direct_fill_stats direct_fill_gpu_minimizer_index(
        const minimizer_hashmap_fragment_index& idx,
        std::span<gpu_minimizer_slot> slots,
        std::span<std::uint64_t> positions) {
    const std::uint64_t distinct = idx.table_size();
    const std::uint32_t capacity = gpu_slot_capacity(distinct);
    if (slots.size() != capacity || positions.size() != idx.positions.size()) {
        throw gpu_index_direct_fill_invariant{};
    }
    if (!slots.empty()) {
        std::memset(slots.data(), 0, slots.size_bytes());
    }
    if (!positions.empty()) {
        std::memcpy(positions.data(), idx.positions.data(), positions.size_bytes());
    }

    gpu_index_direct_fill_stats stats{
        .unique_key_count = distinct,
        .slot_capacity = capacity,
        .load_ppm = gpu_index_load_ppm(distinct, capacity),
    };
    const std::uint32_t mask = capacity - 1u;
    std::uint64_t inserted = 0u;
    idx.for_each_table_entry([&idx, &slots, &stats, &inserted, &mask, &capacity](
                                 std::uint64_t hash, std::uint64_t packed) {
        const std::uint64_t off64 = packed >> 32;
        const std::uint64_t cnt64 = packed & 0xffffffffULL;
        if (cnt64 == 0u
            || off64 > (std::numeric_limits<std::uint32_t>::max)()
            || cnt64 > (std::numeric_limits<std::uint32_t>::max)()
            || off64 > idx.positions.size()
            || cnt64 > idx.positions.size() - off64) {
            throw gpu_index_direct_fill_invariant{};
        }
        std::uint32_t slot_index =
            static_cast<std::uint32_t>(splitmix64(hash)) & mask;
        for (std::uint32_t probe = 1u; probe <= capacity; ++probe) {
            gpu_minimizer_slot& slot = slots[slot_index];
            if (slot.pos_count == 0u) {
                slot.hash = hash;
                slot.pos_offset = static_cast<std::uint32_t>(off64);
                slot.pos_count = static_cast<std::uint32_t>(cnt64);
                stats.max_probe = std::max(stats.max_probe, probe);
                ++inserted;
                return;
            }
            if (slot.hash == hash) {
                throw gpu_index_direct_fill_invariant{};
            }
            slot_index = (slot_index + 1u) & mask;
        }
        throw gpu_index_direct_fill_invariant{};
    });
    if (inserted != distinct || stats.load_ppm > 700'000u) {
        throw gpu_index_direct_fill_invariant{};
    }
    return stats;
}

/// Build an owning host upload image from the CPU index's authoritative buckets and positions.
///
/// Hashes use 70-percent-load open addressing. Position order and encoded 64-bit values are
/// copied unchanged.
/// @throws gpu_index_too_large When device offsets or counts cannot fit 32 bits.
[[nodiscard]] inline gpu_minimizer_index_upload
build_gpu_minimizer_upload(const minimizer_hashmap_fragment_index& idx) {
    if (idx.positions.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw gpu_index_too_large{};
    }
    const std::uint32_t capacity = gpu_slot_capacity(idx.table_size());
    gpu_minimizer_index_upload upload;
    upload.slots.resize(capacity);
    upload.positions = idx.positions;
    upload.target_len = idx.target_len;
    upload.table_size = idx.table_size();
    upload.slot_mask = capacity - 1u;

    idx.for_each_table_entry([&idx, &upload](std::uint64_t hash, std::uint64_t packed) {
        const std::uint64_t off64 = packed >> 32;
        const std::uint64_t cnt64 = packed & 0xffffffffULL;
        if (off64 > std::numeric_limits<std::uint32_t>::max() ||
            cnt64 > std::numeric_limits<std::uint32_t>::max() ||
            off64 + cnt64 > idx.positions.size()) {
            throw gpu_index_too_large{};
        }
        const std::uint32_t off = static_cast<std::uint32_t>(off64);
        const std::uint32_t cnt = static_cast<std::uint32_t>(cnt64);
        std::uint32_t p = static_cast<std::uint32_t>(splitmix64(hash)) & upload.slot_mask;
        for (;;) {
            gpu_minimizer_slot& slot = upload.slots[p];
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

/// Sort hash-position pairs by hash and build an owning open-addressed upload image.
///
/// The input span is reordered. Positions within an equal-hash group retain the radix sort's
/// resulting order and are copied into one contiguous posting interval.
/// @throws gpu_index_too_large When device offsets or counts cannot fit 32 bits.
[[nodiscard]] inline gpu_minimizer_index_upload
build_gpu_minimizer_upload_from_pairs(
    std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
    sequence_size_t target_length)
{
    const std::size_t n = pairs.size();
    if (n > std::numeric_limits<std::uint32_t>::max()) {
        throw gpu_index_too_large{};
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
    gpu_minimizer_index_upload upload;
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
            throw gpu_index_too_large{};
        }
        std::uint32_t p =
            static_cast<std::uint32_t>(splitmix64(hash)) & upload.slot_mask;
        for (;;) {
            gpu_minimizer_slot& slot = upload.slots[p];
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

/// Resident device footprint (held by `gpu_resident_minimizer_index::permit`).
[[nodiscard]] inline std::uint64_t gpu_index_resident_bytes(
    const minimizer_hashmap_fragment_index& idx,
    gpu_index_representation representation =
        gpu_index_representation::open_addressed_70)
{
    return plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(), representation).resident_bytes;
}

/// Direct-fill materialization has no full host upload vectors and no staging
/// buffer.  Resident bytes are admitted separately, so transient upload bytes
/// are exactly zero (returning resident bytes here would double-reserve them).
[[nodiscard]] inline std::uint64_t gpu_index_upload_peak_bytes(
    const minimizer_hashmap_fragment_index&)
{
    return 0u;
}

} // namespace bioseq::indexing
