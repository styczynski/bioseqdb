/**
 * @file gpu_minimizer_index.cppm
 * @brief GPU-resident minimizer index and batch-scoped upload cache.
 *
 * Reuses the existing serialized bucket ABI (`encoded_bucket`) as the GPU slot
 * record; derives either an open-addressed or hash-sorted dense control array
 * plus the positions array at upload time. Keeps
 * `minimizer_hashmap.cppm` storage-free. The cache is owned by the outer
 * query-batch/segment wave; selected Vulkan seed execution treats upload
 * failures and 32-bit ABI overflow as hard `vulkan_runtime_error`s.
 */
module;

#include <libassert/assert.hpp>

#include <bit>
#include <algorithm>
#include <array>
#include <atomic>
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

export module bioseq.indexing.gpu_minimizer_index;

import bioseq.common.checked_arithmetic;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.storage.size;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.common.radix_sort;
import bioseq.common.logging;

export namespace bioseq::indexing {

using gpu_minimizer_slot = minimizer_hashmap_fragment_index::encoded_bucket;
static_assert(sizeof(gpu_minimizer_slot) == 16);
static_assert(offsetof(gpu_minimizer_slot, hash) == 0);
static_assert(offsetof(gpu_minimizer_slot, pos_offset) == 8);
static_assert(offsetof(gpu_minimizer_slot, pos_count) == 12);

// Raw-target host upload object. The persisted path below cannot accept
// it and publishes only fully owned streamed residents through insert_resident.
struct gpu_minimizer_index_upload {
    std::vector<gpu_minimizer_slot> slots;
    std::vector<std::uint64_t> positions;
    std::uint64_t target_len = 0;
    std::uint64_t table_size = 0;
    std::uint32_t slot_mask = 0;
};

struct gpu_index_too_large final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "GPU minimizer index exceeds 32-bit device slot limits";
    }
};

struct gpu_index_direct_fill_invariant final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "GPU minimizer index direct-fill invariant failed";
    }
};

/// Typed diagnostic for a source-direct CPU-built minimizer-index upload. Probe
/// callers may continue to use the nullable handle, while strict pipeline
/// callers pass an output record and preserve the exact predicate that made
/// the handle unavailable.  This record is stats only; it does not permit a
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

enum class gpu_index_representation : std::uint8_t {
    open_addressed_70,
    source_direct_sorted_dense,
};

/// ABI revision of the active source-direct sorted-dense device layout.
inline constexpr std::uint32_t
    k_minimizer_sorted_dense_gpu_abi_revision = 1u;

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

    [[nodiscard]] constexpr bool is_declined() const noexcept {
        return reason != gpu_index_upload_decline_reason::none;
    }

    [[nodiscard]] constexpr bool is_accepted() const noexcept {
        return !is_declined();
    }
};

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

// Raw-target/recompute cache key. It is intentionally not accepted by
// grouped-SoA persisted API below. The pointer-shaped route is confined to
// source-direct batch ownership.
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

[[nodiscard]] inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

struct gpu_minimizer_index_cache_key_hash {
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

[[nodiscard]] inline gpu_minimizer_index_cache_key make_gpu_index_cache_key(
    const minimizer_hashmap_fragment_index& idx,
    gpu_index_cache_identity identity) noexcept
{
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

[[nodiscard]] inline std::uint64_t
next_gpu_source_direct_operation_identity() noexcept {
    static std::atomic<std::uint64_t> next_identity{1u};
    std::uint64_t identity = next_identity.fetch_add(
        1u, std::memory_order_relaxed);
    if (identity == 0u) {
        identity = next_identity.fetch_add(1u, std::memory_order_relaxed);
    }
    return identity;
}

template <class p_minimizer>
[[nodiscard]] inline gpu_index_cache_identity
make_gpu_source_direct_sorted_dense_identity(
    const bioseq::gpu::vulkan::runtime& runtime,
    std::uint64_t operation_identity,
    std::uint64_t minimizer_count,
    std::uint64_t distinct_count,
    std::uint32_t alphabet_size,
    std::uint32_t kmer_size,
    std::uint32_t window_size) noexcept
{
    using layout = typename p_minimizer::layout_type;
    return gpu_index_cache_identity{
        .assembly_identity = 0u,
        .table_size = distinct_count,
        .position_count = minimizer_count,
        .operation_identity = operation_identity,
        .alphabet_size = alphabet_size,
        .kmer_size = kmer_size,
        .window_size = window_size,
        .minimizer_size = sizeof(p_minimizer),
        .minimizer_alignment = alignof(p_minimizer),
        .minimizer_hash_bits = layout::k_hash_bits,
        .minimizer_position_bits = layout::k_position_bits,
        .sorted_dense_abi_revision =
            k_minimizer_sorted_dense_gpu_abi_revision,
        .representation = gpu_index_representation::source_direct_sorted_dense,
        .device_uuid = runtime.features().device_uuid,
        .source_direct_sorted_dense = true,
        .canonical_empty = minimizer_count == 0u
            && distinct_count == 0u,
    };
}

/// Return whether a target sketch tuple is compatible with the fixed
/// source-direct sorted-dense device ABI.  Route selection and published
/// identity validation share this predicate so an ineligible large target
/// remains on the collector/CPU-index route instead of entering a transaction
/// that must reject it later.
[[nodiscard]] inline constexpr bool gpu_source_direct_tuple_supported(
    std::uint32_t alphabet_size,
    std::int64_t kmer_size,
    std::int64_t window_size,
    std::size_t minimizer_size,
    std::size_t minimizer_alignment,
    std::uint32_t minimizer_hash_bits,
    std::uint32_t minimizer_position_bits) noexcept
{
    return (alphabet_size == 4u || alphabet_size == 5u)
        && kmer_size == 15
        && window_size == 10
        && minimizer_size == 8u
        && minimizer_alignment == 8u
        && minimizer_hash_bits == 30u
        && minimizer_position_bits == 33u;
}

/// Typed form used before the target sketch is materialized.  The layout is
/// inspected at compile time without adding a new dispatch/template axis.
template <class p_minimizer>
[[nodiscard]] inline constexpr bool gpu_source_direct_tuple_supported(
    std::uint32_t alphabet_size,
    std::int64_t kmer_size,
    std::int64_t window_size) noexcept
{
    using layout = typename p_minimizer::layout_type;
    return gpu_source_direct_tuple_supported(
        alphabet_size,
        kmer_size,
        window_size,
        sizeof(p_minimizer),
        alignof(p_minimizer),
        layout::k_hash_bits,
        layout::k_position_bits);
}

[[nodiscard]] inline bool gpu_source_direct_identity_well_formed(
    const gpu_index_cache_identity& identity) noexcept
{
    const bool device_uuid_bound = std::any_of(
        identity.device_uuid.begin(), identity.device_uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
    const bool empty = identity.table_size == 0u
        && identity.position_count == 0u;
    const bool nonempty = identity.table_size != 0u
        && identity.position_count != 0u
        && identity.table_size <= identity.position_count;
    return identity.source_direct_sorted_dense
        && identity.operation_identity != 0u
        && (empty != nonempty)
        && identity.canonical_empty == empty
        && gpu_source_direct_tuple_supported(
            identity.alphabet_size,
            identity.kmer_size,
            identity.window_size,
            identity.minimizer_size,
            identity.minimizer_alignment,
            identity.minimizer_hash_bits,
            identity.minimizer_position_bits)
        && identity.sorted_dense_abi_revision
            == k_minimizer_sorted_dense_gpu_abi_revision
        && identity.representation == gpu_index_representation::source_direct_sorted_dense
        && device_uuid_bound
        && !identity.gate_enabled
        && identity.gate_word_count == 0u
        && identity.gate_mask == 0u
        && identity.gate_digest_lo == 0u
        && identity.gate_digest_hi == 0u
        && !identity.gate_canonical_words;
}

enum class gpu_resident_accounting_authority : std::uint8_t {
    none,
    reflected_process_footprint,
    retained_exact_permit,
};

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

[[nodiscard]] inline std::uint32_t gpu_slot_capacity(std::uint64_t distinct) {
    // Power-of-two open addressing at <= 70% load.  ceil(10*D/7) is
    // deliberately evaluated only after an overflow guard; P=93,308,896
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

[[nodiscard]] inline std::uint32_t gpu_index_load_ppm(
        std::uint64_t distinct,
        std::uint64_t capacity) noexcept {
    if (capacity == 0u) return 0u;
    const std::uint64_t scaled =
        bioseq::common::saturating_mul(distinct, 1'000'000u);
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

struct gpu_index_direct_fill_stats {
    std::uint64_t unique_key_count = 0;
    std::uint64_t slot_capacity = 0;
    std::uint32_t load_ppm = 0;
    std::uint32_t max_probe = 0;
};

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

inline constexpr VkBufferUsageFlags k_gpu_source_direct_buffer_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

struct gpu_source_direct_sorted_shape {
    std::uint64_t minimizer_count = 0u;
    std::uint64_t distinct_count = 0u;
};

struct gpu_source_direct_preflight {
    gpu_source_direct_sorted_shape shape{};
    std::uint64_t control_bytes = 0u;
    std::uint64_t position_bytes = 0u;
    std::uint64_t logical_resident_bytes = 0u;
    std::uint64_t control_allocation_requirement_bytes = 0u;
    std::uint64_t position_allocation_requirement_bytes = 0u;
    std::uint64_t control_allocation_allowance_bytes = 0u;
    std::uint64_t position_allocation_allowance_bytes = 0u;
    std::uint64_t allocation_requirement_bytes = 0u;
    std::uint64_t allowance_bytes = 0u;
    std::uint64_t conservative_request_bytes = 0u;
    bool canonical_empty = false;
};

enum class gpu_source_direct_materialization_error : std::uint8_t {
    invalid_shape,
    representation_ineligible,
    device_abi_overflow,
    storage_buffer_range_exceeded,
    resident_memory_admission_denied,
    non_host_visible_device_memory,
    invariant_failed,
};

struct gpu_source_direct_materialization_failure {
    gpu_source_direct_materialization_error error =
        gpu_source_direct_materialization_error::invalid_shape;
    bioseq::parallelism::memory_admission_failure admission{};
};

struct gpu_source_direct_materialization_stats {
    gpu_source_direct_sorted_shape shape{};
    std::uint64_t logical_slot_bytes = 0u;
    std::uint64_t logical_position_bytes = 0u;
    std::uint64_t logical_resident_bytes = 0u;
    std::uint64_t control_allocation_requirement_bytes = 0u;
    std::uint64_t position_allocation_requirement_bytes = 0u;
    std::uint64_t control_allocation_allowance_bytes = 0u;
    std::uint64_t position_allocation_allowance_bytes = 0u;
    std::uint64_t conservative_allowance_bytes = 0u;
    std::uint64_t conservative_request_bytes = 0u;
    std::uint64_t actual_slot_allocation_bytes = 0u;
    std::uint64_t actual_position_allocation_bytes = 0u;
    std::uint64_t actual_allocation_bytes = 0u;
    std::uint64_t source_checksum = 0u;
    std::uint64_t mapped_checksum = 0u;
    std::uint64_t fully_touched_bytes = 0u;
    std::uint64_t reflected_footprint_bytes = 0u;
    std::uint64_t retained_permit_bytes = 0u;
    std::uint64_t permit_reconciliations = 0u;
    std::uint64_t complete_source_traversals = 0u;
    std::uint64_t complete_mapped_traversals = 0u;
    gpu_resident_accounting_authority authority =
        gpu_resident_accounting_authority::none;
};

struct gpu_source_direct_materialization_result {
    std::shared_ptr<gpu_resident_minimizer_index> resident;
    gpu_source_direct_materialization_stats stats{};
};

enum class gpu_permit_reconciliation_action : std::uint8_t {
    invalid,
    shrink,
    unchanged,
    grow,
};

[[nodiscard]] constexpr gpu_permit_reconciliation_action
plan_gpu_exact_permit_reconciliation(
    std::uint64_t held_bytes,
    std::uint64_t actual_bytes) noexcept
{
    if (held_bytes == 0u || actual_bytes == 0u) {
        return gpu_permit_reconciliation_action::invalid;
    }
    if (actual_bytes < held_bytes) {
        return gpu_permit_reconciliation_action::shrink;
    }
    if (actual_bytes > held_bytes) {
        return gpu_permit_reconciliation_action::grow;
    }
    return gpu_permit_reconciliation_action::unchanged;
}

[[nodiscard]] inline bool gpu_exact_resident_authority_valid(
    gpu_resident_accounting_authority authority,
    const bioseq::parallelism::memory_materialization_result& materialization,
    bool permit_present,
    std::uint64_t permit_bytes,
    std::uint64_t actual_allocation_sum) noexcept
{
    const std::uint64_t actual =
        materialization.allocation_snapshot.actual_allocation_bytes;
    const bool common = actual != 0u
        && actual_allocation_sum != 0u
        && actual_allocation_sum
            != (std::numeric_limits<std::uint64_t>::max)()
        && actual == actual_allocation_sum
        && materialization.admitted_bytes == actual
        && materialization.allocation_snapshot.fully_touched_bytes == actual
        && materialization.allocation_snapshot.is_host_visible
        && materialization.allocation_snapshot.is_host_coherent;
    const bool reflected = common
        && authority
            == gpu_resident_accounting_authority::reflected_process_footprint
        && !permit_present && permit_bytes == 0u
        && materialization.is_permit_released
        && materialization.allocation_snapshot.is_process_footprint_reflected
        && materialization.reflected_delta_bytes >= actual;
    const bool retained = common
        && authority == gpu_resident_accounting_authority::retained_exact_permit
        && permit_present && permit_bytes == actual
        && !materialization.is_permit_released;
    return reflected != retained;
}

[[nodiscard]] inline std::expected<gpu_source_direct_preflight,
    gpu_source_direct_materialization_error>
plan_gpu_source_direct_sorted_dense_with_requirements(
    std::uint64_t minimizer_count,
    std::uint64_t distinct_count,
    std::uint64_t max_storage_buffer_range,
    std::uint64_t control_allocation_requirement_bytes,
    std::uint64_t position_allocation_requirement_bytes) noexcept
{
    const bool empty = minimizer_count == 0u && distinct_count == 0u;
    const bool nonempty = minimizer_count != 0u && distinct_count != 0u;
    if (empty) {
        if (control_allocation_requirement_bytes != 0u
            || position_allocation_requirement_bytes != 0u) {
            return std::unexpected{
                gpu_source_direct_materialization_error::invalid_shape};
        }
        return gpu_source_direct_preflight{.canonical_empty = true};
    }
    if (!nonempty || distinct_count > minimizer_count) {
        return std::unexpected{
            gpu_source_direct_materialization_error::invalid_shape};
    }
    if (minimizer_count
            > (std::numeric_limits<std::uint32_t>::max)()
        || distinct_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected{
            gpu_source_direct_materialization_error::device_abi_overflow};
    }
    const std::uint64_t control_bytes =
        bioseq::common::saturating_mul(
            distinct_count, sizeof(gpu_minimizer_slot));
    const std::uint64_t position_bytes =
        bioseq::common::saturating_mul(
            minimizer_count, sizeof(std::uint64_t));
    if (control_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || position_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected{
            gpu_source_direct_materialization_error::device_abi_overflow};
    }
    if (!gpu_index_storage_ranges_fit(
            control_bytes, position_bytes, max_storage_buffer_range)) {
        return std::unexpected{gpu_source_direct_materialization_error::
            storage_buffer_range_exceeded};
    }
    const std::uint64_t logical = bioseq::common::saturating_add(
        control_bytes, position_bytes);
    if (logical == (std::numeric_limits<std::uint64_t>::max)()
        || control_allocation_requirement_bytes < control_bytes
        || position_allocation_requirement_bytes < position_bytes) {
        return std::unexpected{
            gpu_source_direct_materialization_error::device_abi_overflow};
    }
    const std::uint64_t control_allowance =
        control_allocation_requirement_bytes - control_bytes;
    const std::uint64_t position_allowance =
        position_allocation_requirement_bytes - position_bytes;
    const std::uint64_t allowance = bioseq::common::saturating_add(
        control_allowance, position_allowance);
    const std::uint64_t allocation_requirement =
        bioseq::common::saturating_add(
            control_allocation_requirement_bytes,
            position_allocation_requirement_bytes);
    if (allowance == (std::numeric_limits<std::uint64_t>::max)()
        || allocation_requirement
            == (std::numeric_limits<std::uint64_t>::max)()
        || allocation_requirement
            != bioseq::common::saturating_add(logical, allowance)) {
        return std::unexpected{
            gpu_source_direct_materialization_error::device_abi_overflow};
    }
    return gpu_source_direct_preflight{
        .shape = {
            .minimizer_count = minimizer_count,
            .distinct_count = distinct_count,
        },
        .control_bytes = control_bytes,
        .position_bytes = position_bytes,
        .logical_resident_bytes = logical,
        .control_allocation_requirement_bytes =
            control_allocation_requirement_bytes,
        .position_allocation_requirement_bytes =
            position_allocation_requirement_bytes,
        .control_allocation_allowance_bytes = control_allowance,
        .position_allocation_allowance_bytes = position_allowance,
        .allocation_requirement_bytes = allocation_requirement,
        .allowance_bytes = allowance,
        .conservative_request_bytes = allocation_requirement,
        .canonical_empty = false,
    };
}

[[nodiscard]] inline constexpr bool gpu_source_direct_preflight_fits(
    const gpu_source_direct_preflight& preflight,
    std::uint64_t allocatable_bytes) noexcept
{
    return preflight.canonical_empty
        ? preflight.conservative_request_bytes == 0u
        : preflight.conservative_request_bytes != 0u
            && preflight.conservative_request_bytes <= allocatable_bytes;
}

[[nodiscard]] inline std::expected<gpu_source_direct_preflight,
    gpu_source_direct_materialization_error>
plan_gpu_source_direct_sorted_dense(
    bioseq::gpu::vulkan::runtime& runtime,
    std::uint64_t minimizer_count,
    std::uint64_t distinct_count)
{
    const std::uint64_t max_storage_buffer_range =
        runtime.features().max_storage_buffer_range;
    const bool nonempty = minimizer_count != 0u && distinct_count != 0u;
    const std::uint64_t control_bytes = nonempty
        ? bioseq::common::saturating_mul(
            distinct_count, sizeof(gpu_minimizer_slot))
        : 0u;
    const std::uint64_t position_bytes = nonempty
        ? bioseq::common::saturating_mul(
            minimizer_count, sizeof(std::uint64_t))
        : 0u;
    const auto logical_preflight =
        plan_gpu_source_direct_sorted_dense_with_requirements(
            minimizer_count,
            distinct_count,
            max_storage_buffer_range,
            control_bytes,
            position_bytes);
    if (!logical_preflight || logical_preflight->canonical_empty) {
        return logical_preflight;
    }
    const std::uint64_t control_requirement =
        runtime.buffer_allocation_requirement(
            logical_preflight->control_bytes,
            k_gpu_source_direct_buffer_usage);
    const std::uint64_t position_requirement =
        runtime.buffer_allocation_requirement(
            logical_preflight->position_bytes,
            k_gpu_source_direct_buffer_usage);
    return plan_gpu_source_direct_sorted_dense_with_requirements(
        minimizer_count,
        distinct_count,
        max_storage_buffer_range,
        control_requirement,
        position_requirement);
}

template <class p_minimizer>
[[nodiscard]] inline std::expected<gpu_source_direct_sorted_shape,
    gpu_source_direct_materialization_error>
inspect_presorted_minimizer_source(
    std::span<const p_minimizer> source) noexcept
{
    const std::uint64_t minimizer_count = source.size();
    if (minimizer_count == 0u) return gpu_source_direct_sorted_shape{};
    if (minimizer_count
        > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected{
            gpu_source_direct_materialization_error::device_abi_overflow};
    }
    std::uint64_t distinct_count = 1u;
    std::uint64_t previous_hash = source.front().kmer();
    for (std::size_t i = 1u; i < source.size(); ++i) {
        const std::uint64_t hash = source[i].kmer();
        if (hash < previous_hash) {
            return std::unexpected{
                gpu_source_direct_materialization_error::invalid_shape};
        }
        if (hash != previous_hash) ++distinct_count;
        previous_hash = hash;
    }
    if (distinct_count == 0u
        || distinct_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected{
            gpu_source_direct_materialization_error::device_abi_overflow};
    }
    return gpu_source_direct_sorted_shape{
        .minimizer_count = minimizer_count,
        .distinct_count = distinct_count,
    };
}

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
/// Vulkan allocation.  The CPU index remains authoritative; this function
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
    idx.for_each_table_entry([&](std::uint64_t hash, std::uint64_t packed) {
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

/// Construct the hash-sorted dense representation directly in the final
/// mapped Vulkan allocation.  The in-place radix sort uses no full-size
/// staging allocation.  Position offsets remain valid because only controls
/// move; the authoritative position array is copied byte-for-byte.
[[nodiscard]] inline gpu_index_direct_fill_stats
direct_fill_gpu_minimizer_index_sorted_dense(
        const minimizer_hashmap_fragment_index& idx,
        std::span<gpu_minimizer_slot> slots,
        std::span<std::uint64_t> positions) {
    const std::uint64_t distinct = idx.table_size();
    if (distinct == 0u
        || distinct > (std::numeric_limits<std::uint32_t>::max)()
        || slots.size() != distinct
        || positions.size() != idx.positions.size()) {
        throw gpu_index_direct_fill_invariant{};
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
            throw gpu_index_direct_fill_invariant{};
        }
        slots[static_cast<std::size_t>(inserted++)] = gpu_minimizer_slot{
            .hash = hash,
            .pos_offset = static_cast<std::uint32_t>(off64),
            .pos_count = static_cast<std::uint32_t>(cnt64),
        };
    });
    if (inserted != distinct) {
        throw gpu_index_direct_fill_invariant{};
    }
    bioseq::sorting::radix_sort(
        slots.data(), slots.data() + slots.size(),
        [](const gpu_minimizer_slot& slot) noexcept { return slot.hash; });
    for (std::size_t i = 1; i < slots.size(); ++i) {
        if (slots[i - 1u].hash >= slots[i].hash) {
            throw gpu_index_direct_fill_invariant{};
        }
    }
    return {
        .unique_key_count = distinct,
        .slot_capacity = distinct,
        .load_ppm = 1'000'000u,
        .max_probe = 0u,
    };
}

[[nodiscard]] inline std::uint64_t gpu_source_direct_checksum_word(
        std::uint64_t checksum,
        std::uint64_t word) noexcept {
    return splitmix64(checksum ^ splitmix64(word));
}

inline constexpr std::uint64_t k_gpu_source_direct_checksum_seed =
    0x534f555243454450ULL;

template <class p_minimizer>
[[nodiscard]] inline gpu_source_direct_materialization_stats
direct_fill_gpu_minimizer_index_from_presorted_source(
    std::span<const p_minimizer> source,
    bioseq::storage::sequence_size_t target_len,
    gpu_source_direct_sorted_shape shape,
    std::span<gpu_minimizer_slot> controls,
    std::span<std::uint64_t> positions)
{
    if (shape.minimizer_count == 0u || shape.distinct_count == 0u
        || source.size() != shape.minimizer_count
        || controls.size() != shape.distinct_count
        || positions.size() != shape.minimizer_count
        || target_len == 0u) {
        throw gpu_index_direct_fill_invariant{};
    }

    std::uint64_t source_checksum = k_gpu_source_direct_checksum_seed;
    std::size_t source_offset = 0u;
    std::size_t control_index = 0u;
    std::uint64_t previous_hash = 0u;
    bool have_previous_hash = false;
    while (source_offset < source.size()) {
        const std::uint64_t hash = source[source_offset].kmer();
        if (have_previous_hash && hash <= previous_hash) {
            throw gpu_index_direct_fill_invariant{};
        }
        const std::size_t group_begin = source_offset;
        source_checksum = gpu_source_direct_checksum_word(
            source_checksum, hash);
        source_checksum = gpu_source_direct_checksum_word(
            source_checksum, group_begin);
        while (source_offset < source.size()
            && source[source_offset].kmer() == hash) {
            const std::uint64_t encoded =
                source[source_offset].pos_strand_encoded();
            if ((encoded >> 1u)
                >= static_cast<std::uint64_t>(target_len)) {
                throw gpu_index_direct_fill_invariant{};
            }
            positions[source_offset] = encoded;
            source_checksum = gpu_source_direct_checksum_word(
                source_checksum, encoded);
            ++source_offset;
        }
        const std::uint64_t count = source_offset - group_begin;
        if (control_index >= controls.size() || count == 0u
            || group_begin
                > (std::numeric_limits<std::uint32_t>::max)()
            || count > (std::numeric_limits<std::uint32_t>::max)()) {
            throw gpu_index_direct_fill_invariant{};
        }
        controls[control_index] = gpu_minimizer_slot{
            .hash = hash,
            .pos_offset = static_cast<std::uint32_t>(group_begin),
            .pos_count = static_cast<std::uint32_t>(count),
        };
        source_checksum = gpu_source_direct_checksum_word(
            source_checksum, count);
        previous_hash = hash;
        have_previous_hash = true;
        ++control_index;
    }
    if (control_index != controls.size()
        || source_offset != positions.size()) {
        throw gpu_index_direct_fill_invariant{};
    }

    std::uint64_t mapped_checksum = k_gpu_source_direct_checksum_seed;
    std::uint64_t covered_positions = 0u;
    previous_hash = 0u;
    have_previous_hash = false;
    for (const gpu_minimizer_slot& control : controls) {
        const std::uint64_t offset = control.pos_offset;
        const std::uint64_t count = control.pos_count;
        if (count == 0u || offset != covered_positions
            || offset > positions.size()
            || count > positions.size() - offset
            || (have_previous_hash && control.hash <= previous_hash)) {
            throw gpu_index_direct_fill_invariant{};
        }
        mapped_checksum = gpu_source_direct_checksum_word(
            mapped_checksum, control.hash);
        mapped_checksum = gpu_source_direct_checksum_word(
            mapped_checksum, offset);
        for (std::uint64_t position_index = offset;
             position_index < offset + count; ++position_index) {
            const std::uint64_t encoded =
                positions[static_cast<std::size_t>(position_index)];
            if ((encoded >> 1u)
                >= static_cast<std::uint64_t>(target_len)) {
                throw gpu_index_direct_fill_invariant{};
            }
            mapped_checksum = gpu_source_direct_checksum_word(
                mapped_checksum, encoded);
        }
        mapped_checksum = gpu_source_direct_checksum_word(
            mapped_checksum, count);
        covered_positions += count;
        previous_hash = control.hash;
        have_previous_hash = true;
    }
    if (covered_positions != positions.size()
        || source_checksum != mapped_checksum) {
        throw gpu_index_direct_fill_invariant{};
    }
    return gpu_source_direct_materialization_stats{
        .shape = shape,
        .source_checksum = source_checksum,
        .mapped_checksum = mapped_checksum,
        .complete_source_traversals = 1u,
        .complete_mapped_traversals = 1u,
    };
}

template <class p_minimizer>
[[nodiscard]] inline std::expected<
    gpu_source_direct_materialization_result,
    gpu_source_direct_materialization_failure>
materialize_sorted_dense_from_presorted_minimizers(
    bioseq::gpu::vulkan::runtime& runtime,
    std::span<const p_minimizer> source,
    bioseq::storage::sequence_size_t target_len,
    const gpu_source_direct_preflight& preflight,
    const gpu_minimizer_index_cache_key& key,
    bioseq::parallelism::memory_permit& resident_permit)
{
    using layout = typename p_minimizer::layout_type;
    if constexpr (sizeof(p_minimizer) != 8u
        || alignof(p_minimizer) != 8u
        || !std::is_standard_layout_v<p_minimizer>
        || !std::is_trivially_copyable_v<p_minimizer>
        || layout::k_storage_bits != 64u
        || layout::k_hash_bits != 30u
        || layout::k_position_bits != 33u) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::
                representation_ineligible}};
    }
    auto inspected = inspect_presorted_minimizer_source(source);
    if (!inspected) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = inspected.error()}};
    }
    const gpu_source_direct_sorted_shape shape = *inspected;
    const bool device_uuid_bound = std::any_of(
        key.device_uuid.begin(), key.device_uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
    if (shape.minimizer_count == 0u || shape.distinct_count == 0u
        || preflight.canonical_empty
        || preflight.shape.minimizer_count != shape.minimizer_count
        || preflight.shape.distinct_count != shape.distinct_count
        || key.target_len != static_cast<std::uint64_t>(target_len)
        || key.position_count != shape.minimizer_count
        || key.table_size != shape.distinct_count
        || !key.source_direct_sorted_dense
        || key.operation_identity == 0u
        || key.source_id != 0u
        || key.caller_identity != key.operation_identity
        || (key.alphabet_size != 4u && key.alphabet_size != 5u)
        || key.kmer_size != 15u || key.window_size != 10u
        || key.minimizer_size != sizeof(p_minimizer)
        || key.minimizer_alignment != alignof(p_minimizer)
        || key.minimizer_hash_bits != layout::k_hash_bits
        || key.minimizer_position_bits != layout::k_position_bits
        || key.representation != gpu_index_representation::source_direct_sorted_dense
        || key.sorted_dense_abi_revision
            != k_minimizer_sorted_dense_gpu_abi_revision
        || !device_uuid_bound
        || key.device_uuid != runtime.features().device_uuid
        || key.gate_enabled || key.gate_word_count != 0u
        || key.gate_mask != 0u || key.gate_digest_lo != 0u
        || key.gate_digest_hi != 0u || key.gate_canonical_words) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invalid_shape}};
    }

    gpu_index_materialization_plan plan{};
    try {
        plan = plan_gpu_index_materialization(
            shape.distinct_count,
            shape.minimizer_count,
            gpu_index_representation::source_direct_sorted_dense);
    } catch (const gpu_index_too_large&) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::
                device_abi_overflow}};
    }
    if (!gpu_index_storage_ranges_fit(
            plan.slot_buffer_bytes,
            plan.position_buffer_bytes,
            runtime.features().max_storage_buffer_range)) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::
                storage_buffer_range_exceeded}};
    }
    if (preflight.control_bytes != plan.slot_bytes
        || preflight.position_bytes != plan.position_bytes
        || preflight.logical_resident_bytes != plan.resident_bytes
        || preflight.control_allocation_requirement_bytes
            < preflight.control_bytes
        || preflight.position_allocation_requirement_bytes
            < preflight.position_bytes
        || preflight.control_allocation_allowance_bytes
            != preflight.control_allocation_requirement_bytes
                - preflight.control_bytes
        || preflight.position_allocation_allowance_bytes
            != preflight.position_allocation_requirement_bytes
                - preflight.position_bytes
        || preflight.allowance_bytes
            != bioseq::common::saturating_add(
                preflight.control_allocation_allowance_bytes,
                preflight.position_allocation_allowance_bytes)
        || preflight.allocation_requirement_bytes
            != bioseq::common::saturating_add(
                preflight.control_allocation_requirement_bytes,
                preflight.position_allocation_requirement_bytes)
        || preflight.conservative_request_bytes
            != preflight.allocation_requirement_bytes) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    gpu_source_direct_materialization_stats stats{
        .shape = shape,
        .logical_slot_bytes = plan.slot_bytes,
        .logical_position_bytes = plan.position_bytes,
        .logical_resident_bytes = plan.resident_bytes,
        .control_allocation_requirement_bytes =
            preflight.control_allocation_requirement_bytes,
        .position_allocation_requirement_bytes =
            preflight.position_allocation_requirement_bytes,
        .control_allocation_allowance_bytes =
            preflight.control_allocation_allowance_bytes,
        .position_allocation_allowance_bytes =
            preflight.position_allocation_allowance_bytes,
        .conservative_allowance_bytes = preflight.allowance_bytes,
        .conservative_request_bytes =
            preflight.allocation_requirement_bytes,
    };
    if (!resident_permit
        || resident_permit.bytes() != stats.conservative_request_bytes) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::
                resident_memory_admission_denied}};
    }

    auto resident = std::make_shared<gpu_resident_minimizer_index>();
    bool allocation_requirement_mismatch = false;
    bool non_host_visible = false;
    auto* const runtime_ptr = &runtime;
    try {
        const auto materialization =
            bioseq::parallelism::memory_admission_controller::instance().materialize(
                resident_permit,
                [resident, source, target_len, shape, plan, preflight,
                 runtime_ptr,
                 &resident_permit, &stats,
                 &allocation_requirement_mismatch, &non_host_visible]()
                    -> bioseq::parallelism::memory_materialization_snapshot {
                    resident->slots = runtime_ptr->create_device_buffer(
                        plan.slot_buffer_bytes,
                        k_gpu_source_direct_buffer_usage);
                    resident->positions = runtime_ptr->create_device_buffer(
                        plan.position_buffer_bytes,
                        k_gpu_source_direct_buffer_usage);
                    if (resident->slots.allocation_bytes
                            != preflight.
                                control_allocation_requirement_bytes
                        || resident->positions.allocation_bytes
                            != preflight.
                                position_allocation_requirement_bytes) {
                        allocation_requirement_mismatch = true;
                        resident->positions.reset();
                        resident->slots.reset();
                        return {};
                    }
                    const std::uint64_t actual_allocation_bytes =
                        bioseq::common::saturating_add(
                            resident->slots.allocation_bytes,
                            resident->positions.allocation_bytes);
                    const auto reconciliation =
                        plan_gpu_exact_permit_reconciliation(
                            resident_permit.bytes(),
                            actual_allocation_bytes);
                    if (reconciliation
                            != gpu_permit_reconciliation_action::unchanged
                        || actual_allocation_bytes
                            != preflight.allocation_requirement_bytes) {
                        allocation_requirement_mismatch = true;
                        resident->positions.reset();
                        resident->slots.reset();
                        return {};
                    }
                    ++stats.permit_reconciliations;
                    stats.actual_slot_allocation_bytes =
                        resident->slots.allocation_bytes;
                    stats.actual_position_allocation_bytes =
                        resident->positions.allocation_bytes;
                    stats.actual_allocation_bytes = actual_allocation_bytes;
                    if (resident->slots.mapped == nullptr
                        || resident->positions.mapped == nullptr
                        || !resident->slots.is_host_visible_coherent()
                        || !resident->positions.is_host_visible_coherent()) {
                        non_host_visible = true;
                        return {
                            .actual_allocation_bytes = actual_allocation_bytes,
                            .is_host_visible = false,
                            .is_host_coherent = false,
                            .is_process_footprint_reflected = false,
                        };
                    }

                    const auto controls = std::span<gpu_minimizer_slot>{
                        static_cast<gpu_minimizer_slot*>(
                            resident->slots.mapped),
                        static_cast<std::size_t>(shape.distinct_count)};
                    const auto positions = std::span<std::uint64_t>{
                        static_cast<std::uint64_t*>(
                            resident->positions.mapped),
                        static_cast<std::size_t>(shape.minimizer_count)};
                    const auto validation =
                        direct_fill_gpu_minimizer_index_from_presorted_source(
                            source, target_len, shape, controls, positions);
                    stats.source_checksum = validation.source_checksum;
                    stats.mapped_checksum = validation.mapped_checksum;
                    stats.complete_source_traversals =
                        validation.complete_source_traversals;
                    stats.complete_mapped_traversals =
                        validation.complete_mapped_traversals;
                    if (resident->slots.allocation_bytes > plan.slot_bytes) {
                        std::memset(
                            static_cast<std::byte*>(resident->slots.mapped)
                                + plan.slot_bytes,
                            0,
                            static_cast<std::size_t>(
                                resident->slots.allocation_bytes
                                    - plan.slot_bytes));
                    }
                    if (resident->positions.allocation_bytes
                        > plan.position_bytes) {
                        std::memset(
                            static_cast<std::byte*>(resident->positions.mapped)
                                + plan.position_bytes,
                            0,
                            static_cast<std::size_t>(
                                resident->positions.allocation_bytes
                                    - plan.position_bytes));
                    }
                    return {
                        .actual_allocation_bytes = actual_allocation_bytes,
                        .fully_touched_bytes = actual_allocation_bytes,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected =
                            resident->slots.process_footprint_reflected
                            && resident->positions.
                                process_footprint_reflected,
                    };
                });
        resident->materialization = materialization;
    } catch (const gpu_index_direct_fill_invariant&) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    if (allocation_requirement_mismatch) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    if (non_host_visible) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::
                non_host_visible_device_memory}};
    }

    stats.fully_touched_bytes =
        resident->materialization.allocation_snapshot.fully_touched_bytes;
    if (resident->materialization.is_permit_released) {
        stats.authority = gpu_resident_accounting_authority::
            reflected_process_footprint;
        stats.reflected_footprint_bytes =
            resident->materialization.reflected_delta_bytes;
    } else if (resident_permit
               && resident_permit.bytes()
                    == stats.actual_allocation_bytes) {
        stats.authority =
            gpu_resident_accounting_authority::retained_exact_permit;
        stats.retained_permit_bytes = resident_permit.bytes();
        resident->permit = std::move(resident_permit);
    } else {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    resident->accounting_authority = stats.authority;
    resident->key = key;
    resident->gate_identity = gpu_index_gate_identity_from_key(key);
    resident->target_len = target_len;
    resident->slot_mask = 0u;
    resident->position_count =
        static_cast<std::uint32_t>(shape.minimizer_count);
    resident->bucket_count =
        static_cast<std::uint32_t>(shape.distinct_count);
    resident->unique_key_count = shape.distinct_count;
    resident->slot_capacity = shape.distinct_count;
    resident->load_ppm = 1'000'000u;
    resident->max_probe = 0u;
    resident->representation = gpu_index_representation::source_direct_sorted_dense;
    resident->staging_bytes = 0u;
    resident->source_stream_checksum = stats.source_checksum;
    resident->mapped_stream_checksum = stats.mapped_checksum;
    resident->complete_source_traversals =
        stats.complete_source_traversals;
    resident->complete_mapped_traversals =
        stats.complete_mapped_traversals;
    if (!gpu_exact_resident_authority_valid(
            resident->accounting_authority,
            resident->materialization,
            static_cast<bool>(resident->permit),
            resident->permit.bytes(),
            stats.actual_allocation_bytes)) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    return gpu_source_direct_materialization_result{
        .resident = std::move(resident),
        .stats = stats,
    };
}

enum class gpu_built_index_adoption_error : std::uint8_t {
    invalid_build,
    missing_ownership,
    stats_mismatch,
};

[[nodiscard]] inline constexpr std::string_view
gpu_built_index_adoption_error_name(
        gpu_built_index_adoption_error error) noexcept {
    switch (error) {
        case gpu_built_index_adoption_error::invalid_build:
            return "invalid_build";
        case gpu_built_index_adoption_error::missing_ownership:
            return "missing_ownership";
        case gpu_built_index_adoption_error::stats_mismatch:
            return "stats_mismatch";
    }
    return "unknown";
}

/// Zero-allocation ownership handoff for a runtime-built index. The original
/// count/main admission lineage either retains an exact-R permit or carries a
/// strict footprint-backed release commit; adoption never reserves again.
[[nodiscard]] inline std::expected<
    std::shared_ptr<gpu_resident_minimizer_index>, gpu_built_index_adoption_error>
adopt_gpu_built_index(
    bioseq::gpu::vulkan::runtime& runtime,
    bioseq::gpu::vulkan::gpu_built_index&& built,
    gpu_minimizer_index_cache_key key)
{
    if (!built.valid) {
        return std::unexpected{gpu_built_index_adoption_error::invalid_build};
    }
    const std::uint64_t required_position_bytes =
        bioseq::common::saturating_mul(built.position_count, 8u);
    const std::uint64_t required_bucket_bytes =
        bioseq::common::saturating_mul(built.occupied_slots, 16u);
    if (!built.slots.is_valid() || !built.positions.is_valid()
        || built.slots.owner != &runtime || built.positions.owner != &runtime
        || built.slots.mapped == nullptr || built.positions.mapped == nullptr
        || !built.slots.is_host_visible_coherent()
        || !built.positions.is_host_visible_coherent()
        || built.slots.allocation_bytes == 0u
        || built.positions.allocation_bytes == 0u
        || required_position_bytes > built.positions.bytes
        || required_bucket_bytes > built.slots.bytes
        || built.slot_mask != 0u
        || built.occupied_slots > built.position_count
        || built.position_count > built.target_len
        || (built.position_count == 0u) != (built.occupied_slots == 0u)) {
        return std::unexpected{gpu_built_index_adoption_error::invalid_build};
    }
    const std::uint64_t bytes =
        bioseq::common::saturating_add(
            built.slots.allocation_bytes, built.positions.allocation_bytes);
    const auto key_gate_identity = gpu_index_gate_identity_from_key(key);
    if (!built.gate_identity.is_well_formed()
        || !key_gate_identity.is_well_formed()
        || !built.gate_identity.is_exactly_equal_to(key_gate_identity)) {
        return std::unexpected{gpu_built_index_adoption_error::stats_mismatch};
    }
    const auto& commit = built.materialization;
    if (commit.admitted_bytes != bytes
        || commit.allocation_snapshot.actual_allocation_bytes != bytes
        || commit.allocation_snapshot.fully_touched_bytes < bytes
        || !commit.allocation_snapshot.is_host_visible
        || !commit.allocation_snapshot.is_host_coherent
        || built.ownership.resident_bytes != bytes
        || built.ownership.retained_minimizer_count != built.position_count
        || built.ownership.distinct_minimizer_count != built.occupied_slots
        || built.ownership.peak_admitted_bytes != built.peak_admission_bytes
        || built.ownership.peak_observed_bytes
            > built.ownership.peak_admitted_bytes
        || built.ownership.materialization_admitted_bytes
            != commit.admitted_bytes
        || built.ownership.materialization_actual_allocation_bytes
            != commit.allocation_snapshot.actual_allocation_bytes
        || built.ownership.materialization_fully_touched_bytes
            != commit.allocation_snapshot.fully_touched_bytes
        || built.ownership.materialization_reflected_delta_bytes
            != commit.reflected_delta_bytes
        || built.ownership.materialization_permit_released
            != static_cast<std::uint64_t>(commit.is_permit_released)
        || built.ownership.materialization_host_visible
            != static_cast<std::uint64_t>(
                commit.allocation_snapshot.is_host_visible)
        || built.ownership.materialization_host_coherent
            != static_cast<std::uint64_t>(
                commit.allocation_snapshot.is_host_coherent)
        || built.ownership.materialization_process_footprint_reflected
            != static_cast<std::uint64_t>(
                commit.allocation_snapshot.is_process_footprint_reflected)) {
        return std::unexpected{gpu_built_index_adoption_error::stats_mismatch};
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
        && built.ownership.host_sequence_capacity_releases_before_execution
            == (built.ownership.host_sequence_capacity_released_bytes != 0u
                    ? 1u : 0u)
        && built.ownership.direct_tile_arena_fills == 1u
        && built.ownership.host_tile_staging_elided_bytes != 0u
        && built.ownership.count_submit_host_transient_bytes
            == built.ownership.host_sequence_capacity_released_bytes
        && built.ownership.execution_submit_host_transient_bytes == 0u
        && built.ownership.host_transient_released_before_execution_bytes
            == bioseq::common::saturating_add(
                built.ownership.host_sequence_capacity_released_bytes,
                built.ownership.host_tile_staging_elided_bytes)
        && built.ownership.arena_trimmed_before_permit_shrink == 1u;
    if (!target_build_route_matches) {
        return std::unexpected{gpu_built_index_adoption_error::stats_mismatch};
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
        return std::unexpected{gpu_built_index_adoption_error::stats_mismatch};
    }
    const bool retained = static_cast<bool>(built.permit)
        && built.permit.bytes() == bytes
        && !commit.is_permit_released;
    const bool strictly_released = !static_cast<bool>(built.permit)
        && commit.is_permit_released
        && commit.allocation_snapshot.fully_touched_bytes >= bytes
        && commit.allocation_snapshot.is_host_visible
        && commit.allocation_snapshot.is_host_coherent
        && commit.allocation_snapshot.is_process_footprint_reflected
        && commit.reflected_delta_bytes >= bytes;
    if (!retained && !strictly_released) {
        return std::unexpected{gpu_built_index_adoption_error::missing_ownership};
    }
    if (built.ownership.count_submit_admissions != 1u
        || built.ownership.execution_peak_admissions != 1u
        || built.ownership.resident_transfers != 0u
        || built.ownership.adoption_rereserve_attempts != 0u
        || built.ownership.adoption_successes != 0u) {
        return std::unexpected{gpu_built_index_adoption_error::stats_mismatch};
    }

    // Allocate the small owner object before moving any resource, so a host
    // allocation failure leaves `built` completely intact.
    auto resident = std::make_shared<gpu_resident_minimizer_index>();
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
        ? gpu_index_representation::source_direct_sorted_dense
        : gpu_index_representation::open_addressed_70;
    built.valid = false;
    return resident;
}

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

    idx.for_each_table_entry([&](std::uint64_t hash, std::uint64_t packed) {
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

/// Batch/wave-scoped GPU minimizer index cache. Owned by the outer query
/// batch/segment wave; uploads each distinct index once, reuses the resident
/// handle across queries that share it, and evicts LRU when the resident byte
/// budget is exceeded.
class gpu_minimizer_index_cache {
public:
    explicit gpu_minimizer_index_cache(std::uint64_t resident_limit_bytes)
        : resident_limit_bytes_(resident_limit_bytes) {}

    enum class gate_bind_error : std::uint8_t {
        cache_not_empty,
        invalid_identity,
        identity_mismatch,
    };
    enum class built_insert_error : std::uint8_t {
        null_handle,
        resident_limit,
        gate_identity_mismatch,
        resident_key_mismatch,
        duplicate_direct_key,
    };
    enum class required_preinsert_lookup_error : std::uint8_t {
        invalid_key,
        miss,
        resident_mismatch,
    };

    struct gate_identity {
        bool enabled = false;
        std::uint32_t word_count = 0;
        std::uint32_t mask = 0;
        std::uint64_t digest_lo = 0;
        std::uint64_t digest_hi = 0;
        // The digest is only a prefilter.  The immutable canonical words are
        // the cache-identity authority, so even a forced 128-bit collision
        // cannot publish or retrieve an index built for another query wave.
        std::shared_ptr<const std::vector<std::uint32_t>> canonical_words{};

        [[nodiscard]] bioseq::gpu::vulkan::gpu_index_gate_identity
        runtime_identity() const noexcept {
            return bioseq::gpu::vulkan::gpu_index_gate_identity{
                .enabled = enabled,
                .word_count = word_count,
                .mask = mask,
                .digest_lo = digest_lo,
                .digest_hi = digest_hi,
                .canonical_words = canonical_words,
            };
        }

        [[nodiscard]] bool is_well_formed() const noexcept {
            return runtime_identity().is_well_formed();
        }

        [[nodiscard]] bool metadata_matches(
            const gpu_minimizer_index_cache_key& key) const noexcept
        {
            return runtime_identity().is_exactly_equal_to(
                gpu_index_gate_identity_from_key(key));
        }

        [[nodiscard]] bool operator==(const gate_identity& other) const {
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

    [[nodiscard]] std::expected<void, gate_bind_error> bind_gate_once(
            gate_identity identity) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!identity.is_well_formed()) {
            return std::unexpected{gate_bind_error::invalid_identity};
        }
        if (gate_binding_) {
            return *gate_binding_ == identity
                ? std::expected<void, gate_bind_error>{}
                : std::unexpected{gate_bind_error::identity_mismatch};
        }
        if (!entries_.empty()) {
            return std::unexpected{gate_bind_error::cache_not_empty};
        }
        gate_binding_ = identity;
        return {};
    }

    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> get_or_upload(
        bioseq::gpu::vulkan::runtime& runtime,
        const minimizer_hashmap_fragment_index& idx,
        const gpu_minimizer_index_cache_key& key,
        bool cache_enabled,
        gpu_index_upload_decline* decline_out = nullptr)
    {
        if (decline_out != nullptr) {
            *decline_out = gpu_index_upload_decline{};
        }
        if (key.source_direct_sorted_dense) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "source-direct sorted-dense keys require preinsert lookup");
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

        std::shared_ptr<const gpu_resident_minimizer_index> handle;
        try {
            handle = upload_locked_index(runtime, idx, key, decline_out);
        } catch (const gpu_index_too_large&) {
            if (decline_out != nullptr) {
                decline_out->reason =
                    gpu_index_upload_decline_reason::device_abi_overflow;
                return nullptr;
            }
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan minimizer index exceeds the 32-bit device ABI");
        } catch (const gpu_index_direct_fill_invariant&) {
            if (decline_out != nullptr) {
                decline_out->reason = gpu_index_upload_decline_reason::
                    direct_fill_invariant_failed;
                return nullptr;
            }
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan minimizer index direct-fill invariant failed");
        } catch (const bioseq::gpu::vulkan::vulkan_runtime_error&) {
            throw;
        }
        if (!handle) return nullptr;

        if (cache_enabled) {
            insert_with_eviction_locked(key, handle);
        }
        return handle;
    }

    /// Insert a GPU-built resident index adopted from a device
    /// index build) under `key` so the seed stage finds it instead of uploading
    /// the CPU `minimizer_hashmap_fragment_index`.
    [[nodiscard]] std::expected<void, built_insert_error> insert_built(
        const gpu_minimizer_index_cache_key& key,
        std::shared_ptr<const gpu_resident_minimizer_index> handle)
    {
        if (!handle) {
            return std::unexpected{built_insert_error::null_handle};
        }
        std::lock_guard<std::mutex> guard(mu_);
        if (!key_gate_matches_binding_locked(key)) {
            return std::unexpected{
                built_insert_error::gate_identity_mismatch};
        }
        const auto key_gate_identity = gpu_index_gate_identity_from_key(key);
        if (!handle->gate_identity.is_well_formed()
            || !handle->gate_identity.is_exactly_equal_to(key_gate_identity)
            || (gate_binding_
                && !handle->gate_identity.is_exactly_equal_to(
                    gate_binding_->runtime_identity()))) {
            return std::unexpected{
                built_insert_error::gate_identity_mismatch};
        }
        if (!(handle->key == key)) {
            return std::unexpected{built_insert_error::resident_key_mismatch};
        }
        if (key.source_direct_sorted_dense && entries_.contains(key)) {
            return std::unexpected{built_insert_error::duplicate_direct_key};
        }
        const std::uint64_t handle_bytes = handle_resident_bytes(*handle);
        if (resident_limit_bytes_ != 0u
            && handle_bytes > resident_limit_bytes_) {
            return std::unexpected{built_insert_error::resident_limit};
        }
        insert_with_eviction_locked(key, handle);
        return {};
    }

    /// Required lookup for an operation-owned, already-published resident.
    /// This API has no CPU-index argument by construction, so a miss can never
    /// transcode or upload an empty placeholder. Runtime is read-only identity
    /// authority for the buffer owners and selected device UUID.
    [[nodiscard]] std::expected<
        std::shared_ptr<const gpu_resident_minimizer_index>,
        required_preinsert_lookup_error>
    find_required_preinserted(
        const bioseq::gpu::vulkan::runtime& runtime,
        const gpu_minimizer_index_cache_key& key)
    {
        std::lock_guard<std::mutex> guard(mu_);
        const bool device_uuid_bound = std::any_of(
            key.device_uuid.begin(), key.device_uuid.end(),
            [](std::uint8_t byte) noexcept { return byte != 0u; });
        if (!key.source_direct_sorted_dense
            || key.operation_identity == 0u
            || key.source_id != 0u
            || key.caller_identity != key.operation_identity
            || (key.alphabet_size != 4u && key.alphabet_size != 5u)
            || key.kmer_size != 15u || key.window_size != 10u
            || key.minimizer_size != 8u
            || key.minimizer_alignment != 8u
            || key.minimizer_hash_bits != 30u
            || key.minimizer_position_bits != 33u
            || !device_uuid_bound
            || key.device_uuid != runtime.features().device_uuid
            || key.representation != gpu_index_representation::source_direct_sorted_dense
            || key.sorted_dense_abi_revision
                != k_minimizer_sorted_dense_gpu_abi_revision
            || key.table_size == 0u || key.position_count == 0u
            || key.table_size > key.position_count
            || key.gate_enabled || !key_gate_matches_binding_locked(key)) {
            return std::unexpected{
                required_preinsert_lookup_error::invalid_key};
        }
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            return std::unexpected{required_preinsert_lookup_error::miss};
        }
        const auto& resident = found->second;
        const std::uint64_t required_control_bytes =
            bioseq::common::saturating_mul(
                key.table_size, sizeof(gpu_minimizer_slot));
        const std::uint64_t required_position_bytes =
            bioseq::common::saturating_mul(
                key.position_count, sizeof(std::uint64_t));
        const std::uint64_t actual_allocation_sum =
            bioseq::common::saturating_add(
                resident ? resident->slots.allocation_bytes : 0u,
                resident ? resident->positions.allocation_bytes : 0u);
        if (!resident || !(resident->key == key)
            || resident->representation
                != gpu_index_representation::source_direct_sorted_dense
            || resident->slot_mask != 0u
            || resident->bucket_count != key.table_size
            || resident->unique_key_count != key.table_size
            || resident->slot_capacity != key.table_size
            || resident->position_count != key.position_count
            || resident->target_len != key.target_len
            || resident->gate_identity.enabled
            || resident->accounting_authority
                == gpu_resident_accounting_authority::none
            || !resident->slots.is_valid() || !resident->positions.is_valid()
            || resident->slots.owner != &runtime
            || resident->positions.owner != &runtime
            || resident->slots.mapped == nullptr
            || resident->positions.mapped == nullptr
            || !resident->slots.is_host_visible_coherent()
            || !resident->positions.is_host_visible_coherent()
            || required_control_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || required_position_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || required_control_bytes > resident->slots.bytes
            || required_position_bytes > resident->positions.bytes
            || required_control_bytes > resident->slots.allocation_bytes
            || required_position_bytes > resident->positions.allocation_bytes
            || resident->source_stream_checksum == 0u
            || resident->source_stream_checksum
                != resident->mapped_stream_checksum
            || resident->complete_source_traversals != 1u
            || resident->complete_mapped_traversals != 1u
            || !gpu_exact_resident_authority_valid(
                resident->accounting_authority,
                resident->materialization,
                static_cast<bool>(resident->permit),
                resident->permit.bytes(),
                actual_allocation_sum)) {
            return std::unexpected{
                required_preinsert_lookup_error::resident_mismatch};
        }
        touch_lru_locked(key);
        return resident;
    }

    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> insert_upload(
        bioseq::gpu::vulkan::runtime& runtime,
        gpu_minimizer_index_upload&& upload,
        const gpu_minimizer_index_cache_key& key,
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

    void erase(const gpu_minimizer_index_cache_key& key) noexcept {
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
        const gpu_minimizer_index_cache_key& key) const noexcept
    {
        if (gate_binding_) {
            return gate_binding_->is_well_formed()
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
        const gpu_minimizer_index_cache_key& key) const
    {
        if (!key_gate_matches_binding_locked(key)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan minimizer-index cache gate identity mismatch");
        }
    }

    [[nodiscard]] static std::uint64_t handle_resident_bytes(
        const gpu_resident_minimizer_index& handle) noexcept
    {
        return bioseq::common::saturating_add(
            handle.slots.allocation_bytes,
            handle.positions.allocation_bytes);
    }

    void touch_lru_locked(const gpu_minimizer_index_cache_key& key) {
        auto pos = std::find(lru_.begin(), lru_.end(), key);
        if (pos != lru_.end()) {
            lru_.splice(lru_.begin(), lru_, pos);
        }
    }

    void insert_with_eviction_locked(
        const gpu_minimizer_index_cache_key& key,
        const std::shared_ptr<const gpu_resident_minimizer_index>& handle)
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
            const gpu_minimizer_index_cache_key victim = lru_.back();
            lru_.pop_back();
            auto found = entries_.find(victim);
            if (found != entries_.end()) {
                resident_bytes_ -= handle_resident_bytes(*found->second);
                entries_.erase(found);
            }
        }
        entries_[key] = handle;
        lru_.push_front(key);
        resident_bytes_ = bioseq::common::saturating_add(
            resident_bytes_, handle_bytes);
    }

    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> upload_locked_index(
        bioseq::gpu::vulkan::runtime& runtime,
        const minimizer_hashmap_fragment_index& idx,
        const gpu_minimizer_index_cache_key& key,
        gpu_index_upload_decline* decline_out)
    {
        using bioseq::parallelism::memory_admission_controller;
        if (idx.positions.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
            throw gpu_index_too_large{};
        }
        const std::uint64_t upload_peak = gpu_index_upload_peak_bytes(idx);
        const std::uint64_t unique_key_count = idx.table_size();
        const gpu_index_materialization_plan open_plan =
            plan_gpu_index_materialization(
                unique_key_count, idx.positions.size(),
                gpu_index_representation::open_addressed_70);
        std::optional<gpu_index_materialization_plan> dense_plan;
        if (unique_key_count != 0u
            && unique_key_count
                <= (std::numeric_limits<std::uint32_t>::max)()) {
            dense_plan = plan_gpu_index_materialization(
                unique_key_count, idx.positions.size(),
                gpu_index_representation::source_direct_sorted_dense);
        }
        const std::uint64_t max_storage_buffer_range =
            runtime.features().max_storage_buffer_range;
        const auto ranges_fit = [max_storage_buffer_range](
                const gpu_index_materialization_plan& candidate) noexcept {
            return gpu_index_storage_ranges_fit(
                candidate.slot_buffer_bytes,
                candidate.position_buffer_bytes,
                max_storage_buffer_range);
        };
        gpu_index_materialization_plan plan = open_plan;
        if (!ranges_fit(open_plan) && dense_plan) {
            plan = *dense_plan;
        }
        const auto publish_plan = [&](
                const gpu_index_materialization_plan& selected) {
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
                decline_out->reason = gpu_index_upload_decline_reason::
                    storage_buffer_range_exceeded;
            }
            return nullptr;
        }

        auto& admission_controller = memory_admission_controller::instance();
        bioseq::parallelism::memory_admission_failure admission_failure{};
        auto resident_permit =
            admission_controller.try_reserve(
                plan.resident_bytes, &admission_failure);
        if (!resident_permit
            && plan.representation
                == gpu_index_representation::open_addressed_70
            && dense_plan
            && dense_plan->resident_bytes < plan.resident_bytes
            && ranges_fit(*dense_plan)) {
            const auto open_failure = admission_failure;
            plan = *dense_plan;
            publish_plan(plan);
            admission_failure = {};
            bioseq_log<logging_level::info>(
                "[gpu_index_representation_adaptation] from={} to={} "
                "open_resident_bytes={} dense_resident_bytes={} "
                "open_admission_reason={} open_budget_ceiling_bytes={} "
                "open_allocatable_after_reservations_bytes={}",
                gpu_index_representation_name(
                    gpu_index_representation::open_addressed_70),
                gpu_index_representation_name(
                    gpu_index_representation::source_direct_sorted_dense),
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
                decline_out->reason = gpu_index_upload_decline_reason::
                    resident_memory_admission_denied;
                decline_out->admission = admission_failure;
            }
            return nullptr;
        }

        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto resident = std::make_shared<gpu_resident_minimizer_index>();
        gpu_index_direct_fill_stats fill_stats{};
        bool allocation_growth_denied = false;
        std::uint64_t denied_actual_allocation_bytes = 0u;
        bool non_host_visible = false;
        bioseq::parallelism::memory_admission_failure growth_failure{};
        const auto materialization = admission_controller.materialize(
            *resident_permit,
            [&]() -> bioseq::parallelism::memory_materialization_snapshot {
                resident->slots = runtime.create_device_buffer(
                    plan.slot_buffer_bytes, usage);
                resident->positions = runtime.create_device_buffer(
                    plan.position_buffer_bytes, usage);
                const std::uint64_t actual_allocation_bytes =
                    bioseq::common::saturating_add(
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
                        .is_host_visible = false,
                        .is_host_coherent = false,
                        .is_process_footprint_reflected = false,
                    };
                }

                const auto mapped_slots = std::span<gpu_minimizer_slot>{
                    static_cast<gpu_minimizer_slot*>(resident->slots.mapped),
                    static_cast<std::size_t>(plan.slot_capacity)};
                const auto mapped_positions = std::span<std::uint64_t>{
                    static_cast<std::uint64_t*>(resident->positions.mapped),
                    idx.positions.size()};
                fill_stats = plan.representation
                        == gpu_index_representation::source_direct_sorted_dense
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
                    .is_host_visible = host_visible,
                    .is_host_coherent = host_coherent,
                    .is_process_footprint_reflected =
                        resident->slots.process_footprint_reflected
                        && resident->positions.process_footprint_reflected,
                };
            });
        if (allocation_growth_denied) {
            if (decline_out != nullptr) {
                decline_out->reason = gpu_index_upload_decline_reason::
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
                decline_out->reason = gpu_index_upload_decline_reason::
                    non_host_visible_device_memory;
            }
            return nullptr;
        }
        resident->permit = std::move(*resident_permit);
        resident->key = key;
        resident->gate_identity = gpu_index_gate_identity_from_key(key);
        resident->target_len = idx.target_len;
        resident->slot_mask = plan.representation
                == gpu_index_representation::open_addressed_70
            ? static_cast<std::uint32_t>(plan.slot_capacity - 1u)
            : 0u;
        resident->position_count = static_cast<std::uint32_t>(idx.positions.size());
        resident->bucket_count = plan.representation
                == gpu_index_representation::source_direct_sorted_dense
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
                materialization.allocation_snapshot.actual_allocation_bytes;
            decline_out->fully_touched_bytes =
                materialization.allocation_snapshot.fully_touched_bytes;
            decline_out->reflected_footprint_delta_bytes =
                materialization.reflected_delta_bytes;
            decline_out->materialization_permit_released =
                materialization.is_permit_released;
            decline_out->process_footprint_reflected =
                materialization.allocation_snapshot
                    .is_process_footprint_reflected;
        }
        bioseq_log<logging_level::info>(
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
            materialization.allocation_snapshot.actual_allocation_bytes,
            materialization.allocation_snapshot.fully_touched_bytes,
            materialization.is_permit_released ? 1 : 0,
            materialization.allocation_snapshot
                .is_process_footprint_reflected ? 1 : 0,
            materialization.reflected_delta_bytes);
        return resident;
    }

    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> upload_prepared_locked(
        bioseq::gpu::vulkan::runtime& runtime,
        const gpu_minimizer_index_upload& upload,
        const gpu_minimizer_index_cache_key& key)
    {
        using bioseq::parallelism::memory_admission_controller;
        const std::uint64_t slot_bytes =
            static_cast<std::uint64_t>(upload.slots.size()) * sizeof(gpu_minimizer_slot);
        const std::uint64_t pos_bytes =
            static_cast<std::uint64_t>(upload.positions.size()) * sizeof(std::uint64_t);
        const std::uint64_t resident_bytes =
            bioseq::common::saturating_add(slot_bytes, pos_bytes);
        auto resident_permit =
            memory_admission_controller::instance().try_reserve(resident_bytes);
        if (!resident_permit) return nullptr;

        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto resident = std::make_shared<gpu_resident_minimizer_index>();
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
    std::optional<gate_identity> gate_binding_{};
    mutable std::mutex mu_;
    std::list<gpu_minimizer_index_cache_key> lru_;
    std::unordered_map<gpu_minimizer_index_cache_key,
        std::shared_ptr<const gpu_resident_minimizer_index>,
        gpu_minimizer_index_cache_key_hash> entries_;
};

// Grouped-SoA restart-cold resident ingestion.

[[nodiscard]] inline bool is_gpu_device_uuid_bound(
        const bioseq::gpu::vulkan::gpu_device_uuid& uuid) noexcept {
    return std::any_of(
        uuid.begin(), uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
}

/** Failures exposed by grouped-SoA resident ingestion and publication. */
enum class gpu_grouped_soa_ingest_error : std::uint8_t {
    invalid_identity,
    device_identity_mismatch,
    unsupported_endianness,
    metadata_mismatch,
    arithmetic_overflow,
    admission,
    non_mappable_device_memory,
    partition_mismatch,
    byte_count,
    checksum,
    completion_mismatch,
    unauthorized_extent,
    poisoned_stream,
};

static_assert(k_grouped_soa_gpu_abi_revision
    == bioseq::gpu::vulkan::k_gpu_grouped_soa_abi_revision);

/// Immutable identity of one restart-cold grouped-SoA upload. The opaque
/// cold_load_identity is supplied by the PostgreSQL/scheduler owner; it is not
/// derived from a host pointer or from query contents.
struct gpu_grouped_soa_resident_identity {
    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_checksum = 0u;
    std::uint64_t storage_contract_checksum = 0u;
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
    bioseq::gpu::vulkan::gpu_device_uuid device_uuid{};
    std::uint32_t gpu_abi_revision = 0u;

    [[nodiscard]] bool operator==(
        const gpu_grouped_soa_resident_identity&) const = default;
};

[[nodiscard]] inline bool is_gpu_grouped_soa_resident_identity_valid(
    const gpu_grouped_soa_resident_identity& identity) noexcept
{
    return identity.layout
            == minimizer_generation_layout::
                grouped_soa_segment_local
        && identity.generation_id != 0u
        && identity.source_identity != 0u
        && identity.cold_load_identity != 0u
        && is_gpu_device_uuid_bound(identity.device_uuid)
        && identity.gpu_abi_revision == k_grouped_soa_gpu_abi_revision;
}

/// Positive and prohibited-work stats owned by a completed cold upload.
/// Counts describe bytes copied directly into their final device buffers; no
/// decoded host index or temporary pair array is part of this representation.
struct gpu_grouped_soa_cold_load_stats {
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

class gpu_grouped_soa_generation_ingest;

/// Cacheable, immutable grouped-SoA target. `controls` retains each persisted
/// SoA extent byte-for-byte and `positions` retains the persisted segment-local
/// scalar lanes byte-for-byte.  The descriptor buffer is the only derived
/// device object and contains metadata/byte offsets, never decoded payload.
class gpu_grouped_soa_resident_target {
private:
    struct construction_token {};

public:
    gpu_grouped_soa_resident_target(
        construction_token,
        bioseq::parallelism::memory_permit permit,
        bioseq::gpu::vulkan::buffer controls,
        bioseq::gpu::vulkan::buffer positions,
        bioseq::gpu::vulkan::buffer descriptors,
        gpu_grouped_soa_resident_identity identity,
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

    [[nodiscard]] const bioseq::gpu::vulkan::buffer& controls() const noexcept {
        return controls_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& positions() const noexcept {
        return positions_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& descriptors() const noexcept {
        return descriptors_;
    }
    [[nodiscard]] const gpu_grouped_soa_resident_identity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const gpu_grouped_soa_cold_load_stats& cold_load_stats() const noexcept {
        return stats_;
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

    /// This slice is an index-residency stats, not a scheduler execution stats.
    [[nodiscard]] static constexpr bool
    qualifies_as_all_gpu_execution() noexcept { return false; }
    [[nodiscard]] static constexpr bool
    permits_host_decode_or_rebuild() noexcept { return false; }

    [[nodiscard]] bool validated() const noexcept {
        const std::uint64_t payload_bytes =
            bioseq::common::saturating_add(
                control_bytes_, position_bytes_);
        const std::uint64_t expected_directory_bytes =
            bioseq::common::saturating_mul(
                descriptor_count_,
                grouped_soa_bucket_directory_entry::encoded_bytes);
        const std::uint64_t expected_descriptor_bytes =
            bioseq::common::saturating_mul(
                descriptor_count_,
                sizeof(bioseq::gpu::vulkan::
                    gpu_grouped_soa_resident_partition_descriptor));
        const std::uint64_t allocation_bytes =
            bioseq::common::saturating_add(
                controls_.allocation_bytes,
                bioseq::common::saturating_add(
                    positions_.allocation_bytes,
                    descriptors_.allocation_bytes));
        return is_gpu_grouped_soa_resident_identity_valid(identity_)
            && controls_.is_valid() && positions_.is_valid() && descriptors_.is_valid()
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
            && stats_.cold_cache_misses == 1u
            && stats_.full_generations_ingested == 1u
            && stats_.control_extents_validated == descriptor_count_
            && stats_.position_extents_validated == descriptor_count_
            && stats_.control_bytes_copied == control_bytes_
            && stats_.position_bytes_copied == position_bytes_
            && stats_.descriptor_bytes_copied == descriptor_bytes_
            && stats_.device_allocation_bytes == allocation_bytes
            && stats_.admitted_device_bytes == permit_.bytes()
            && stats_.validated_receipt_rows != 0u
            && stats_.raw_rehashed_rows == 0u
            && stats_.payload_hash_byte_visits == 0u
            && !stats_.has_forbidden_work();
    }

private:
    // First declaration ensures buffers are destroyed before their admission.
    bioseq::parallelism::memory_permit permit_{};
    bioseq::gpu::vulkan::buffer controls_{};
    bioseq::gpu::vulkan::buffer positions_{};
    bioseq::gpu::vulkan::buffer descriptors_{};
    gpu_grouped_soa_resident_identity identity_{};
    gpu_grouped_soa_cold_load_stats stats_{};
    std::uint64_t directory_bytes_ = 0u;
    std::uint64_t control_bytes_ = 0u;
    std::uint64_t position_bytes_ = 0u;
    std::uint64_t descriptor_bytes_ = 0u;
    std::uint64_t control_count_ = 0u;
    std::uint64_t position_count_ = 0u;
    std::uint32_t descriptor_count_ = 0u;

    friend class gpu_grouped_soa_generation_ingest;
};

/// Direct full-generation receiver for the first restart-cold measurement.
/// Requests follow physical grouped-SoA order: every control extent,
/// then every position extent. Chunks are copied once into final resident
/// buffers. PostgreSQL's validated-row receipts supply already-checked row
/// checksums, avoiding a second payload-byte hash pass. The raw entry point is
/// deliberately test-only and cannot publish a resident.
class gpu_grouped_soa_generation_ingest {
public:
    gpu_grouped_soa_generation_ingest(
        const gpu_grouped_soa_generation_ingest&) = delete;
    gpu_grouped_soa_generation_ingest& operator=(
        const gpu_grouped_soa_generation_ingest&) = delete;
    gpu_grouped_soa_generation_ingest(
        gpu_grouped_soa_generation_ingest&&) noexcept = default;
    gpu_grouped_soa_generation_ingest& operator=(
        gpu_grouped_soa_generation_ingest&&) = delete;

    template <class p_directory_view>
    [[nodiscard]] static std::expected<
        gpu_grouped_soa_generation_ingest,
        gpu_grouped_soa_ingest_error>
    begin(
        bioseq::gpu::vulkan::runtime& runtime,
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const expected_minimizer_generation_identity& expected,
        const p_directory_view& directory,
        std::uint64_t cold_load_identity)
    {
        if constexpr (std::endian::native != std::endian::little) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::unsupported_endianness);
        }
        if (!runtime.is_usable()
            || !is_gpu_device_uuid_bound(runtime.features().device_uuid)
            || cold_load_identity == 0u) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::invalid_identity);
        }
        const auto manifest_profile =
            derive_grouped_soa_minimizer_profile(manifest);
        const bool compatible = manifest_profile
            && manifest.layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local
            && manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision
            && directory.profile().layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local
            && directory.profile().gpu_abi_revision
                == k_grouped_soa_gpu_abi_revision
            && directory.profile() == *manifest_profile;
        if (!compatible
            || !validate_minimizer_generation_storage_contract(
                manifest, expected)
            || !validate_minimizer_generation_metadata(
                head, manifest, directory)) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        const auto entries = directory.entries();
        if (entries.size()
                > (std::numeric_limits<std::uint32_t>::max)()
            || entries.size()
                > (std::numeric_limits<std::size_t>::max)() / 2u) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }

        std::vector<std::uint64_t> control_offsets(entries.size());
        std::vector<std::uint64_t> position_offsets(entries.size());
        std::vector<bioseq::gpu::vulkan::
            gpu_grouped_soa_resident_partition_descriptor> descriptors;
        descriptors.reserve(entries.size());
        std::uint64_t control_bytes = 0u;
        std::uint64_t position_bytes = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_count = 0u;
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            if (validate_minimizer_bucket_dir_entry(
                    entry, directory.profile()).has_value()
                || entry.partition_id != ordinal
                || entry.control_element_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                || entry.position_element_count
                    > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::partition_mismatch);
            }
            const auto core = derive_minimizer_position_segment_bounds(
                manifest, static_cast<std::uint32_t>(entry.position_segment));
            const auto layout = plan_grouped_soa_control_plane(
                entry.control_element_count,
                manifest.hash_width,
                entry.shape_offset_width,
                manifest.count_width);
            if (!core || !layout
                || control_bytes
                    % static_cast<std::uint8_t>(
                        manifest.hash_width) != 0u
                || position_bytes
                    % static_cast<std::uint8_t>(
                        manifest.position_width) != 0u
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
                    gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
            control_offsets[ordinal] = control_bytes;
            position_offsets[ordinal] = position_bytes;
            descriptors.push_back(
                bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor{
                    .partition_id = entry.partition_id,
                    .directory_ordinal = static_cast<std::uint32_t>(ordinal),
                    .position_segment = static_cast<std::uint32_t>(
                        entry.position_segment),
                    .flags = bioseq::gpu::vulkan::
                        k_gpu_grouped_soa_partition_required_flags,
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
                        manifest.hash_width),
                    .offset_width = static_cast<std::uint32_t>(
                        entry.shape_offset_width),
                    .count_width = static_cast<std::uint32_t>(
                        manifest.count_width),
                    .position_width = static_cast<std::uint32_t>(
                        manifest.position_width),
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
            bioseq::common::saturating_add(
                control_bytes, position_bytes);
        const std::uint64_t descriptor_bytes =
            bioseq::common::saturating_mul(
                descriptors.size(),
                sizeof(bioseq::gpu::vulkan::
                    gpu_grouped_soa_resident_partition_descriptor));
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
                    * grouped_soa_bucket_directory_entry::encoded_bytes) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::metadata_mismatch);
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
                gpu_grouped_soa_ingest_error::arithmetic_overflow);
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
            bioseq::common::saturating_add(
                control_admission,
                bioseq::common::saturating_add(
                    position_admission, descriptor_admission));
        if (admitted_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(admitted_bytes);
        if (!permit) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::admission);
        }
        auto control_buffer = runtime.create_device_buffer(
            control_request, usage);
        auto position_buffer = runtime.create_device_buffer(
            position_request, usage);
        auto descriptor_buffer = runtime.create_device_buffer(
            descriptor_request, usage);
        const std::uint64_t allocation_bytes =
            bioseq::common::saturating_add(
                control_buffer.allocation_bytes,
                bioseq::common::saturating_add(
                    position_buffer.allocation_bytes,
                    descriptor_buffer.allocation_bytes));
        if (allocation_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || allocation_bytes > admitted_bytes) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::admission);
        }
        if (control_buffer.mapped == nullptr
            || position_buffer.mapped == nullptr
            || descriptor_buffer.mapped == nullptr) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::non_mappable_device_memory);
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

        const gpu_grouped_soa_resident_identity identity{
            .layout = manifest.layout,
            .generation_id = manifest.generation_id,
            .source_identity = manifest.source_identity,
            .source_content_checksum = manifest.source_content_checksum,
            .storage_contract_checksum = head.storage_contract_checksum,
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
            .gpu_abi_revision = manifest.gpu_abi_revision,
        };
        auto resident = std::make_shared<gpu_grouped_soa_resident_target>(
            gpu_grouped_soa_resident_target::construction_token{},
            std::move(*permit), std::move(control_buffer),
            std::move(position_buffer), std::move(descriptor_buffer),
            identity, manifest.directory_bytes, control_bytes,
            position_bytes, descriptor_bytes, control_count,
            position_count, static_cast<std::uint32_t>(entries.size()));
        resident->stats_.descriptor_bytes_copied = descriptor_bytes;

        std::vector<extent_plan> plans;
        std::vector<index_sub_key_request> requests;
        plans.reserve(entries.size() * 2u);
        requests.reserve(entries.size() * 2u);
        const auto append_plan = [entries, &manifest, &control_offsets,
                                  &position_offsets](
                                     std::size_t ordinal, bool position,
                                     auto& plan_out, auto& request_out)
                -> bool {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            const auto position_sub_key =
                derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) return false;
            const index_sub_key_request request{
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
            plan_out.push_back(extent_plan{
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
                    gpu_grouped_soa_ingest_error::partition_mismatch);
            }
        }
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            if (!append_plan(ordinal, true, plans, requests)) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::partition_mismatch);
            }
        }
        std::uint64_t expected_payload_rows = 0u;
        for (const index_sub_key_request& request : requests) {
            if (expected_payload_rows
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - request.expected_chunk_count) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
            expected_payload_rows += request.expected_chunk_count;
        }
        return gpu_grouped_soa_generation_ingest{
            std::move(resident), std::move(plans), std::move(requests),
            manifest.payload_checksum, expected_payload_rows};
    }

    [[nodiscard]] std::span<const index_sub_key_request> requests() const noexcept {
        return requests_;
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_validated_row(
        const validated_minimizer_generation_row_receipt& receipt) noexcept
    {
        std::expected<void, gpu_grouped_soa_ingest_error> accepted{};
        const bool visited = visit_validated_minimizer_generation_row(
            receipt,
            [this, &accepted](
                const index_chunk_address& address,
                std::span<const std::byte> bytes,
                std::uint64_t row_checksum) noexcept {
                accepted = accept_chunk_impl(
                    address, bytes, row_checksum,
                    input_authority::validated_receipt);
            });
        if (!visited) {
            return fail<void>(
                gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        return accepted;
    }

    /// Standalone corruption-test path. Production full-generation loading
    /// must pass validated_minimizer_generation_row_receipt instead.
    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_raw_chunk_for_test(
        const index_chunk_address& address,
        std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const std::uint64_t row_checksum = crc64_index_chunk_row(
            address.generation_id, address.position_segment,
            address.sub_key, address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()), bytes);
        return accept_chunk_impl(
            address, bytes, row_checksum, input_authority::raw_rehashed);
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_raw_chunk_for_test(
        const index_chunk_address& address,
        const char* payload,
        std::size_t payload_bytes) noexcept
    {
        if (payload == nullptr && payload_bytes != 0u) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        return accept_raw_chunk_for_test(address, std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(payload), payload_bytes});
    }

    [[nodiscard]] std::expected<
        std::shared_ptr<const gpu_grouped_soa_resident_target>,
        gpu_grouped_soa_ingest_error>
    finish()
    {
        if (failure_) return std::unexpected(*failure_);
        if (!resident_ || extent_cursor_ != plans_.size()
            || chunk_cursor_ != 0u) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::byte_count);
        }
        // Only the receipt path is publishable. It proves that PostgreSQL's
        // single scan already checked each row while the aggregate below binds
        // the exact ordered checksum sequence to this manifest.
        if (input_authority_ != input_authority::validated_receipt
            || resident_->stats_.validated_receipt_rows
                != expected_payload_rows_
            || resident_->stats_.raw_rehashed_rows != 0u
            || resident_->stats_.payload_hash_byte_visits != 0u) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        if (payload_row_crc_.checksum() != payload_checksum_expected_) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::checksum);
        }
        resident_->stats_.cold_cache_misses = 1u;
        resident_->stats_.full_generations_ingested = 1u;
        resident_->stats_.device_allocation_bytes =
            resident_->controls_.allocation_bytes
            + resident_->positions_.allocation_bytes
            + resident_->descriptors_.allocation_bytes;
        resident_->stats_.admitted_device_bytes = resident_->permit_.bytes();
        if (!resident_->validated()) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        std::shared_ptr<const gpu_grouped_soa_resident_target> published = resident_;
        resident_.reset();
        return published;
    }

private:
    enum class input_authority : std::uint8_t {
        unset,
        validated_receipt,
        raw_rehashed,
    };

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_chunk_impl(
        const index_chunk_address& address,
        std::span<const std::byte> bytes,
        std::uint64_t admitted_row_checksum,
        input_authority authority) noexcept
    {
        if (failure_) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::poisoned_stream);
        }
        if (input_authority_ == input_authority::unset) {
            input_authority_ = authority;
        } else if (input_authority_ != authority) {
            return fail<void>(
                gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        if (!resident_ || extent_cursor_ >= plans_.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const extent_plan& plan = plans_[extent_cursor_];
        const index_sub_key_request& request = plan.request;
        if (chunk_cursor_ >= request.expected_chunk_count
            || address.generation_id != request.generation_id
            || address.position_segment != request.position_segment
            || address.sub_key != request.sub_key
            || address.chunk_no != request.first_chunk_no + chunk_cursor_) {
            return fail<void>(
                gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        const std::uint64_t quantum = k_grouped_soa_extent_payload_bytes;
        const std::uint64_t chunk_offset =
            static_cast<std::uint64_t>(chunk_cursor_) * quantum;
        if (chunk_offset >= request.expected_total_bytes) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const std::uint64_t expected_bytes = std::min<std::uint64_t>(
            quantum, request.expected_total_bytes - chunk_offset);
        bioseq::gpu::vulkan::buffer& destination = plan.position
            ? resident_->positions_ : resident_->controls_;
        const std::uint64_t destination_offset =
            bioseq::common::saturating_add(
                plan.destination_offset, chunk_offset);
        if (bytes.size() != expected_bytes
            || destination_offset
                == (std::numeric_limits<std::uint64_t>::max)()
            || bytes.size() > destination.bytes
            || destination_offset
                > destination.bytes - bytes.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        std::memcpy(
            static_cast<std::byte*>(destination.mapped)
                + destination_offset,
            bytes.data(), bytes.size());
        if (authority == input_authority::raw_rehashed) {
            extent_crc_.update(bytes);
            resident_->stats_.raw_rehashed_rows++;
            resident_->stats_.payload_hash_byte_visits += bytes.size() * 2u;
        } else {
            resident_->stats_.validated_receipt_rows++;
        }
        payload_row_crc_.update_little_endian(admitted_row_checksum);
        if (plan.position) {
            resident_->stats_.position_bytes_copied += bytes.size();
        } else {
            resident_->stats_.control_bytes_copied += bytes.size();
        }
        ++chunk_cursor_;
        if (chunk_cursor_ == request.expected_chunk_count) {
            if (authority == input_authority::raw_rehashed
                && extent_crc_.checksum() != request.expected_checksum) {
                return fail<void>(gpu_grouped_soa_ingest_error::checksum);
            }
            if (plan.position) {
                ++resident_->stats_.position_extents_validated;
            } else {
                ++resident_->stats_.control_extents_validated;
            }
            ++extent_cursor_;
            chunk_cursor_ = 0u;
            extent_crc_ = {};
        }
        return {};
    }
    struct extent_plan {
        index_sub_key_request request{};
        std::uint64_t destination_offset = 0u;
        bool position = false;
    };

    gpu_grouped_soa_generation_ingest(
        std::shared_ptr<gpu_grouped_soa_resident_target> resident,
        std::vector<extent_plan> plans,
        std::vector<index_sub_key_request> requests,
        std::uint64_t payload_checksum_expected,
        std::uint64_t expected_payload_rows) noexcept
        : resident_(std::move(resident)), plans_(std::move(plans)),
          requests_(std::move(requests)),
          payload_checksum_expected_(payload_checksum_expected),
          expected_payload_rows_(expected_payload_rows) {}

    template <class p_value>
    [[nodiscard]] std::expected<p_value, gpu_grouped_soa_ingest_error>
    fail(gpu_grouped_soa_ingest_error error) noexcept {
        failure_ = error;
        return std::unexpected(error);
    }

    std::shared_ptr<gpu_grouped_soa_resident_target> resident_{};
    std::vector<extent_plan> plans_{};
    std::vector<index_sub_key_request> requests_{};
    std::size_t extent_cursor_ = 0u;
    std::uint32_t chunk_cursor_ = 0u;
    crc64_ecma182_accumulator extent_crc_{};
    crc64_ecma182_accumulator payload_row_crc_{};
    std::uint64_t payload_checksum_expected_ = 0u;
    std::uint64_t expected_payload_rows_ = 0u;
    input_authority input_authority_ = input_authority::unset;
    std::optional<gpu_grouped_soa_ingest_error> failure_{};
};

} // namespace bioseq::indexing
