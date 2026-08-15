/**
 * @file hash_sorted_ingest.cppm
 * @brief Source-direct hash-sorted GPU minimizer-index ingestion.
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

export module bioseq.indexing.gpu_minimizer_index:hash_sorted_ingest;

import :direct_ingest;
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
/// Construct the hash-sorted dense representation directly in the final
/// mapped Vulkan allocation.  The in-place radix sort uses no full-size
/// staging allocation.  Position offsets remain valid because only controls
/// move. The authoritative position array is copied byte-for-byte.
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
    idx.for_each_table_entry([&idx, &slots, &inserted](std::uint64_t hash, std::uint64_t packed) {
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
/// strict footprint-backed release commit. Adoption never reserves again.
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

} // namespace bioseq::indexing
