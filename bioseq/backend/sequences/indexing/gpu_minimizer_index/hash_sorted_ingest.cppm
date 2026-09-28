/**
 * @file hash_sorted_ingest.cppm
 * @brief Build sorted minimizer lookup buffers directly from stored or generated minimizers.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
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
import bioseq.pipeline.runtime_contract;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// Assign a nonzero process-local ID to a minimizer build  operation.
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

/// Describe a minimizer build by its operation, record layout, counts, and GPU.
/// @tparam p_minimizer Packed minimizer type that supplies hash and position widths.
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

/// Check that a generated Vulkan profile supports this K/W pair and record layout.
/// The caller must selec the alphabet and packed sequence format before this
/// check. Here alphabet_size is only checked for a valid range because the
/// cache identity does not carry the alphabet  code or packed sequence width.
[[nodiscard]] inline constexpr bool gpu_source_direct_tuple_supported(
    std::uint32_t alphabet_size,
    std::int64_t kmer_size,
    std::int64_t window_size,
    std::size_t minimizer_size,
    std::size_t minimizer_alignment,
    std::uint32_t minimizer_hash_bits,
    std::uint32_t minimizer_position_bits) noexcept
{
    if (alphabet_size == 0u || alphabet_size > 256u || kmer_size <= 0 ||
        kmer_size > static_cast<std::int64_t>((std::numeric_limits<std::uint16_t>::max)()) ||
        window_size <= 0 ||
        window_size > static_cast<std::int64_t>((std::numeric_limits<std::uint16_t>::max)()) ||
        minimizer_size == 0u || minimizer_alignment == 0u ||
        !std::has_single_bit(minimizer_alignment) || minimizer_alignment > minimizer_size ||
        minimizer_hash_bits == 0u || minimizer_position_bits == 0u ||
        minimizer_position_bits > 63u) {
        return false;
    }

    const auto representation_matches = [&](
        const bioseq::minimizers::minimizer_physical_record_descriptor& candidate) {
        const std::size_t expected_alignment =
            std::min<std::size_t>(candidate.record_bytes, alignof(std::uint64_t));
        return candidate.record_bytes == minimizer_size &&
            candidate.storage_bits == minimizer_size * 8u &&
            expected_alignment == minimizer_alignment &&
            candidate.available_hash_bits == minimizer_hash_bits &&
            candidate.position_log2 == minimizer_position_bits;
    };

    const auto runtime_kmer_size = static_cast<std::uint16_t>(kmer_size);
    const auto runtime_window_size = static_cast<std::uint16_t>(window_size);
    for (const auto& candidate :
         bioseq::minimizers::registered_minimizer_runtime_profiles()) {
        const auto* selected = bioseq::minimizers::find_minimizer_runtime_profile(
            bioseq::simd::minimizer_algorithm::vulkan_minimap_integer,
            candidate.alphabet_id,
            runtime_kmer_size,
            runtime_window_size,
            static_cast<std::uint8_t>(minimizer_position_bits),
            candidate.packed_storage_bits);
        if (selected == nullptr ||
            selected->effective_algorithm !=
                bioseq::simd::minimizer_algorithm::vulkan_minimap_integer) {
            continue;
        }
        // Records with the same size can belong to different K/W profiles.
        // Compare the record selected for this profile.
        const auto* physical_record =
            bioseq::minimizers::find_minimizer_physical_record(selected->record_id);
        if (physical_record != nullptr && representation_matches(*physical_record)) {
            return true;
        }
    }
    return false;
}

/// Check profile support using the packed minimizer type's size and bit widths.
/// @tparam p_minimizer Packed minimizer type that defines the record layout.
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

/// Check operation and device IDs, source counts, record layout, and disabled hash filtering.
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

/// Vulkan usage flags for shader access and copies of control and position buffers.
inline constexpr VkBufferUsageFlags k_gpu_source_direct_buffer_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

/// Counts of minimizers and, distinct  hash groups in an index source.
struct gpu_source_direct_sorted_shape {
    std::uint64_t minimizer_count = 0u;
    std::uint64_t distinct_count = 0u;
};

/// Buffer sizes and driver allocation requirements used to reserve memory before building
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

/// Errors while checking sizes, reserving memory, or filling the final GPU buffers.
enum class gpu_source_direct_materialization_error : std::uint8_t {
    invalid_shape,
    representation_ineligible,
    device_abi_overflow,
    storage_buffer_range_exceeded,
    resident_memory_admission_denied,
    non_host_visible_device_memory,
    invariant_failed,
};

/// A build error, with memory-budget details when admission fails.
struct gpu_source_direct_materialization_failure {
    gpu_source_direct_materialization_error error =
        gpu_source_direct_materialization_error::invalid_shape;
    bioseq::parallelism::memory_admission_failure admission{};
};

/// Build counts, checksums, allocation sizes, and the memory budget held for the result.
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

/// The completed GPU index and the checks recorded whil building it.
struct gpu_source_direct_materialization_result {
    std::shared_ptr<gpu_resident_minimizer_index> resident;
    gpu_source_direct_materialization_stats stats{};
};

/// How a memory permit must change to match the  actual allocation.
enum class gpu_permit_reconciliation_action : std::uint8_t {
    invalid,
    shrink,
    unchanged,
    grow,
};

/// Compare reserved and allocated bytes to decide whether the permit must change.
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

/// Check that the measured process footprint or a held permit covers all resident memory.
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

/// Calculate buffer sizes and allocation padding before reserving memory.
///
/// Empty input requires zero allocation bytes. Nonempty input must fit the
/// shader's 32-bit counts and each buffer must fit its Vulkan binding limit.
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

/// Check whether the planned allocation fits the supplied memory budget.
[[nodiscard]] inline constexpr bool gpu_source_direct_preflight_fits(
    const gpu_source_direct_preflight& preflight,
    std::uint64_t allocatable_bytes) noexcept
{
    return preflight.canonical_empty
        ? preflight.conservative_request_bytes == 0u
        : preflight.conservative_request_bytes != 0u
            && preflight.conservative_request_bytes <= allocatable_bytes;
}

/// Ask Vulkan for  the allocation sizes needed by the planned control and position buffers.
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

/// Count distinct hashes and reject minimizers whose hashes are out of order.
/// @tparam p_minimizer Packed minimizer type exposing kmer().
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
/// Copy a CPU index into mapped buffers and sort its controls by hash.
/// Positions keep their original order. Sorting only the controls preserves
/// their offsets into the copied position array and needs no second full array
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

/// Fold one source or mapped word into the traversal checksum.
[[nodiscard]] inline std::uint64_t gpu_source_direct_checksum_word(
        std::uint64_t checksum,
        std::uint64_t word) noexcept {
    return splitmix64(checksum ^ splitmix64(word));
}

/// Initial value shared by source and mapped-data checksums.
inline constexpr std::uint64_t k_gpu_source_direct_checksum_seed =
    0x534f555243454450ULL;

/// Copy a hash-sorted source, into final controls and position intervals.
///
/// Each control names one hesh and its  contiguous positions. The function then
/// rereads the mapped buffers, checks their bounds, and compares their checksum
/// with the checksum collected while copying. Position encodings stay unchanged.
///
/// @tparam p_minimizer Packed type exposing kmer() and pos_strand_encoded().
/// @throws gpu_index_direct_fill_invariant If counts, order, positions, or checksums disagree.
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

/// Build final mapped GPU buffers from a hash-sorted minimizer span.
///
/// The source, cache key, device, and memory plan must agree. The result owns
/// both buffers. It keeps their permit unless the measured process footprint
/// already accounts for the allocation when the buffers are built.
///
/// @tparam p_minimizer Eight-byte minimizer  with 30 hash bits and 33 position bits.
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
                    // CPU prefix searches also read these buffers. Use cached host memory
                    // that remains accessible to Vulkan.
                resident->slots = runtime_ptr->create_host_buffer(
                        plan.slot_buffer_bytes,
                        k_gpu_source_direct_buffer_usage);
                    resident->positions = runtime_ptr->create_host_buffer(
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

/// Build a cache key from the source counts and layout without building a CPU index.
template <class p_minimizer>
[[nodiscard]] inline gpu_minimizer_index_cache_key
make_gpu_mapped_fragment_sorted_dense_key(
    const bioseq::gpu::vulkan::runtime& runtime,
    std::uint64_t operation_identity,
    bioseq::storage::sequence_size_t target_len,
    std::uint64_t distinct_count,
    std::uint64_t position_count,
    std::uint32_t alphabet_size,
    std::uint32_t kmer_size,
    std::uint32_t window_size) noexcept {
    using layout = typename p_minimizer::layout_type;
    return gpu_minimizer_index_cache_key{
        .source_id = 0u,
        .caller_identity = operation_identity,
        .target_len = static_cast<std::uint64_t>(target_len),
        .table_size = distinct_count,
        .position_count = position_count,
        .operation_identity = operation_identity,
        .alphabet_size = alphabet_size,
        .kmer_size = kmer_size,
        .window_size = window_size,
        .minimizer_size = sizeof(p_minimizer),
        .minimizer_alignment = alignof(p_minimizer),
        .minimizer_hash_bits = layout::k_hash_bits,
        .minimizer_position_bits = layout::k_position_bits,
        .sorted_dense_abi_revision = k_minimizer_sorted_dense_gpu_abi_revision,
        .representation = gpu_index_representation::source_direct_sorted_dense,
        .device_uuid = runtime.features().device_uuid,
        .source_direct_sorted_dense = true,
    };
}

/// Build the same cache key for minimizers produced by the Vulkan runtime.
template <class p_minimizer>
[[nodiscard]] inline gpu_minimizer_index_cache_key
make_gpu_runtime_built_sorted_dense_key(
    const bioseq::gpu::vulkan::runtime& runtime,
    std::uint64_t operation_identity,
    bioseq::storage::sequence_size_t target_len,
    std::uint64_t distinct_count,
    std::uint64_t position_count,
    std::uint32_t alphabet_size,
    std::uint32_t kmer_size,
    std::uint32_t window_size) noexcept {
    return make_gpu_mapped_fragment_sorted_dense_key<p_minimizer>(runtime,
                                                                  operation_identity,
                                                                  target_len,
                                                                  distinct_count,
                                                                  position_count,
                                                                  alphabet_size,
                                                                  kmer_size,
                                                                  window_size);
}

namespace segmented_spill_gpu_detail {

template <class p_hash>
struct count_state {
    std::span<const p_hash> selected_hashes{};
    std::span<std::uint64_t> occurrence_counts{};
    std::uint64_t expected_event_count = 0u;
    std::uint64_t target_len = 0u;
    std::uint64_t observed_event_count = 0u;
    std::uint64_t selected_hash_mask = 0u;
    bool valid = true;
};

template <class p_hash>
[[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
count_event(void* raw, const canonical_minimizer_event& event) noexcept {
    auto& state = *static_cast<count_state<p_hash>*>(raw);
    if (!state.valid || (event.hash & ~state.selected_hash_mask) != 0u ||
        event.hash > static_cast<std::uint64_t>((std::numeric_limits<p_hash>::max)()) ||
        (event.encoded_position >> 1u) >= state.target_len ||
        state.observed_event_count >= state.expected_event_count) {
        state.valid = false;
        return std::unexpected(minimizer_generation_encoding_error::position_range);
    }
    const p_hash hash = static_cast<p_hash>(event.hash);
    const auto found = std::lower_bound(
        state.selected_hashes.begin(), state.selected_hashes.end(), hash);
    if (found == state.selected_hashes.end() || *found != hash) {
        state.valid = false;
        return std::unexpected(
            minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const std::size_t index = static_cast<std::size_t>(
        found - state.selected_hashes.begin());
    if (state.occurrence_counts[index] ==
        (std::numeric_limits<std::uint32_t>::max)()) {
        state.valid = false;
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    ++state.occurrence_counts[index];
    ++state.observed_event_count;
    return {};
}

struct fill_state {
    std::span<gpu_minimizer_slot> controls{};
    std::span<std::uint64_t> positions{};
    std::uint64_t expected_event_count = 0u;
    std::uint64_t target_len = 0u;
    std::uint64_t observed_event_count = 0u;
    std::uint64_t selected_hash_mask = 0u;
    bool valid = true;
};

[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
fill_event(void* raw, const canonical_minimizer_event& event) noexcept {
    auto& state = *static_cast<fill_state*>(raw);
    if (!state.valid || state.controls.empty() ||
        (event.hash & ~state.selected_hash_mask) != 0u ||
        (event.encoded_position >> 1u) >= state.target_len ||
        state.observed_event_count >= state.expected_event_count) {
        state.valid = false;
        return std::unexpected(minimizer_generation_encoding_error::position_range);
    }
    const auto found = std::lower_bound(
        state.controls.begin(),
        state.controls.end(),
        event.hash,
        [](const gpu_minimizer_slot& control, std::uint64_t hash) noexcept {
            return control.hash < hash;
        });
    if (found == state.controls.end() || found->hash != event.hash) {
        state.valid = false;
        return std::unexpected(
            minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const std::size_t control_index = static_cast<std::size_t>(
        found - state.controls.begin());
    const std::uint64_t interval_end =
        control_index + 1u < state.controls.size()
            ? state.controls[control_index + 1u].pos_offset
            : state.positions.size();
    const std::uint64_t interval_begin = found->pos_offset;
    const std::uint64_t cursor = found->pos_count;
    if (interval_end <= interval_begin || cursor >= interval_end - interval_begin ||
        interval_begin > state.positions.size() ||
        cursor > state.positions.size() - interval_begin) {
        state.valid = false;
        return std::unexpected(
            minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const std::size_t destination =
        static_cast<std::size_t>(interval_begin + cursor);
    if (destination >= state.positions.size() ||
        (cursor != 0u && state.positions[destination - 1u] >= event.encoded_position)) {
        state.valid = false;
        return std::unexpected(minimizer_generation_encoding_error::duplicate_event);
    }
    state.positions[destination] = event.encoded_position;
    ++found->pos_count;
    ++state.observed_event_count;
    return {};
}

} // namespace segmented_spill_gpu_detail

/**
 * Build one GPU lookup table from the spill selected by all queries in a statement.
 *
 * Replay the spill twice. The first pasb counts occurrences for each selected
 * hash so every hash can receive  a contiguous position interval The second
 * pass writes those intervals directly in the final mapped buffers. The spill
 * stays on disk, and the result is checked for counts and position order.
 */
template <class p_minimizer, class p_hash, class p_replay_producer>
[[nodiscard]] inline std::expected<
    gpu_source_direct_materialization_result,
    gpu_source_direct_materialization_failure>
materialize_sorted_dense_from_segmented_spill_replay(
    bioseq::gpu::vulkan::runtime& runtime,
    p_replay_producer& replay,
    std::span<const p_hash> selected_hashes,
    std::span<std::uint64_t> occurrence_counts,
    std::uint64_t expected_event_count,
    std::uint64_t position_segment_span_bp,
    bioseq::storage::sequence_size_t target_len,
    std::uint64_t operation_identity,
    std::uint32_t alphabet_size,
    std::uint32_t kmer_size,
    std::uint32_t window_size) {
    using layout = typename p_minimizer::layout_type;
    static_assert(std::is_unsigned_v<p_hash>);
    constexpr std::uint32_t selected_hash_bits =
        layout::k_storage_bits == 128u
            ? std::min<std::uint32_t>(layout::k_hash_bits, 56u)
            : layout::k_hash_bits;
    constexpr std::uint64_t selected_hash_mask = []() noexcept {
        if constexpr (selected_hash_bits == 64u) {
            return (std::numeric_limits<std::uint64_t>::max)();
        } else {
            return (std::uint64_t{1u} << selected_hash_bits) - 1u;
        }
    }();
    const bool device_uuid_bound = std::any_of(
        runtime.features().device_uuid.begin(),
        runtime.features().device_uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
    if constexpr (!std::is_standard_layout_v<p_minimizer> ||
                  !std::is_trivially_copyable_v<p_minimizer> ||
                  (sizeof(p_hash) != sizeof(std::uint32_t) &&
                   sizeof(p_hash) != sizeof(std::uint64_t)) ||
                  selected_hash_bits == 0u || selected_hash_bits > 64u ||
                  layout::k_position_bits == 0u || layout::k_position_bits > 63u) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::representation_ineligible}};
    }
    if (!runtime.is_usable() || !device_uuid_bound || target_len == 0u ||
        operation_identity == 0u || selected_hashes.empty() ||
        selected_hashes.size() != occurrence_counts.size() ||
        expected_event_count == 0u || position_segment_span_bp == 0u ||
        expected_event_count > (std::numeric_limits<std::uint32_t>::max)() ||
        replay.scan_count() != 0u || replay.merge_pass_count() != 0u ||
        replay.completed_scan_count() != 0u || replay.failed_scan_count() != 0u ||
        replay.illegal_span_transition_count() != 0u ||
        !gpu_source_direct_tuple_supported<p_minimizer>(
            alphabet_size, kmer_size, window_size)) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invalid_shape}};
    }
    for (std::size_t index = 0u; index < selected_hashes.size(); ++index) {
        const std::uint64_t hash = static_cast<std::uint64_t>(selected_hashes[index]);
        if (occurrence_counts[index] != 0u || (hash & ~selected_hash_mask) != 0u ||
            (index != 0u && selected_hashes[index - 1u] >= selected_hashes[index])) {
            return std::unexpected{gpu_source_direct_materialization_failure{
                .error = gpu_source_direct_materialization_error::invalid_shape}};
        }
    }

    segmented_spill_gpu_detail::count_state<p_hash> count_state{
        .selected_hashes = selected_hashes,
        .occurrence_counts = occurrence_counts,
        .expected_event_count = expected_event_count,
        .target_len = static_cast<std::uint64_t>(target_len),
        .selected_hash_mask = selected_hash_mask,
    };
    const auto counted = replay.scan_sorted_events(
        position_segment_span_bp,
        canonical_minimizer_event_receiver{
            .context = std::addressof(count_state),
            .emit_fn = &segmented_spill_gpu_detail::count_event<p_hash>,
        });
    if (!counted || !count_state.valid ||
        count_state.observed_event_count != expected_event_count ||
        replay.scan_count() != 1u || replay.merge_pass_count() != 1u ||
        replay.completed_scan_count() != 1u || replay.failed_scan_count() != 0u ||
        replay.illegal_span_transition_count() != 0u ||
        replay.visited_event_count() != expected_event_count) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    std::uint64_t distinct_count = 0u;
    std::uint64_t counted_positions = 0u;
    for (const std::uint64_t count : occurrence_counts) {
        if (count != 0u) {
            ++distinct_count;
        }
        counted_positions = bioseq::common::saturating_add(counted_positions, count);
    }
    if (distinct_count == 0u || counted_positions != expected_event_count ||
        distinct_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    auto preflight = plan_gpu_source_direct_sorted_dense(
        runtime, expected_event_count, distinct_count);
    if (!preflight) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = preflight.error()}};
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto resident_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            preflight->conservative_request_bytes, &admission_failure);
    if (!resident_permit) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::resident_memory_admission_denied,
            .admission = admission_failure}};
    }

    auto resident = std::make_shared<gpu_resident_minimizer_index>();
    gpu_source_direct_materialization_stats stats{
        .shape = {
            .minimizer_count = expected_event_count,
            .distinct_count = distinct_count,
        },
        .logical_slot_bytes = preflight->control_bytes,
        .logical_position_bytes = preflight->position_bytes,
        .logical_resident_bytes = preflight->logical_resident_bytes,
        .control_allocation_requirement_bytes =
            preflight->control_allocation_requirement_bytes,
        .position_allocation_requirement_bytes =
            preflight->position_allocation_requirement_bytes,
        .control_allocation_allowance_bytes =
            preflight->control_allocation_allowance_bytes,
        .position_allocation_allowance_bytes =
            preflight->position_allocation_allowance_bytes,
        .conservative_allowance_bytes = preflight->allowance_bytes,
        .conservative_request_bytes = preflight->conservative_request_bytes,
    };
    bool allocation_requirement_mismatch = false;
    bool non_host_visible = false;
    auto* const runtime_ptr = std::addressof(runtime);
    try {
        resident->materialization =
            bioseq::parallelism::memory_admission_controller::instance().materialize(
                *resident_permit,
                [resident,
                 &replay,
                 selected_hashes,
                 occurrence_counts,
                 expected_event_count,
                 position_segment_span_bp,
                 target_len,
                 distinct_count,
                 selected_hash_mask,
                 preflight = *preflight,
                 runtime_ptr,
                 &stats,
                 &allocation_requirement_mismatch,
                 &non_host_visible]() -> bioseq::parallelism::memory_materialization_snapshot {
                    // CPU prefix searches also read these buffers. Use cached host memory
                    // that remains accessible to Vulkan.
                resident->slots = runtime_ptr->create_host_buffer(
                        preflight.control_bytes, k_gpu_source_direct_buffer_usage);
                    resident->positions = runtime_ptr->create_host_buffer(
                        preflight.position_bytes, k_gpu_source_direct_buffer_usage);
                    if (resident->slots.allocation_bytes !=
                            preflight.control_allocation_requirement_bytes ||
                        resident->positions.allocation_bytes !=
                            preflight.position_allocation_requirement_bytes) {
                        allocation_requirement_mismatch = true;
                        resident->positions.reset();
                        resident->slots.reset();
                        return {};
                    }
                    const std::uint64_t actual = bioseq::common::saturating_add(
                        resident->slots.allocation_bytes,
                        resident->positions.allocation_bytes);
                    if (actual != preflight.allocation_requirement_bytes) {
                        allocation_requirement_mismatch = true;
                        resident->positions.reset();
                        resident->slots.reset();
                        return {};
                    }
                    stats.actual_slot_allocation_bytes = resident->slots.allocation_bytes;
                    stats.actual_position_allocation_bytes =
                        resident->positions.allocation_bytes;
                    stats.actual_allocation_bytes = actual;
                    ++stats.permit_reconciliations;
                    if (resident->slots.mapped == nullptr ||
                        resident->positions.mapped == nullptr ||
                        !resident->slots.is_host_visible_coherent() ||
                        !resident->positions.is_host_visible_coherent()) {
                        non_host_visible = true;
                        return {
                            .actual_allocation_bytes = actual,
                            .is_host_visible = false,
                            .is_host_coherent = false,
                            .is_process_footprint_reflected = false,
                        };
                    }
                    auto controls = std::span<gpu_minimizer_slot>{
                        static_cast<gpu_minimizer_slot*>(resident->slots.mapped),
                        static_cast<std::size_t>(distinct_count)};
                    auto positions = std::span<std::uint64_t>{
                        static_cast<std::uint64_t*>(resident->positions.mapped),
                        static_cast<std::size_t>(expected_event_count)};
                    std::memset(resident->slots.mapped,
                                0,
                                static_cast<std::size_t>(resident->slots.allocation_bytes));
                    std::memset(resident->positions.mapped,
                                0,
                                static_cast<std::size_t>(resident->positions.allocation_bytes));

                    std::uint64_t position_offset = 0u;
                    std::size_t control_index = 0u;
                    for (std::size_t index = 0u; index < selected_hashes.size(); ++index) {
                        const std::uint64_t count = occurrence_counts[index];
                        if (count == 0u) {
                            continue;
                        }
                        if (control_index >= controls.size() ||
                            position_offset > (std::numeric_limits<std::uint32_t>::max)() ||
                            count > (std::numeric_limits<std::uint32_t>::max)() ||
                            position_offset > expected_event_count - count) {
                            throw gpu_index_direct_fill_invariant{};
                        }
                        controls[control_index++] = gpu_minimizer_slot{
                            .hash = static_cast<std::uint64_t>(selected_hashes[index]),
                            .pos_offset = static_cast<std::uint32_t>(position_offset),
                            .pos_count = 0u,
                        };
                        position_offset += count;
                    }
                    if (control_index != controls.size() ||
                        position_offset != expected_event_count) {
                        throw gpu_index_direct_fill_invariant{};
                    }

                    segmented_spill_gpu_detail::fill_state fill_state{
                        .controls = controls,
                        .positions = positions,
                        .expected_event_count = expected_event_count,
                        .target_len = static_cast<std::uint64_t>(target_len),
                        .selected_hash_mask = selected_hash_mask,
                    };
                    const auto filled = replay.scan_sorted_events(
                        position_segment_span_bp,
                        canonical_minimizer_event_receiver{
                            .context = std::addressof(fill_state),
                            .emit_fn = &segmented_spill_gpu_detail::fill_event,
                        });
                    const std::uint64_t expected_visited =
                        bioseq::common::saturating_mul(expected_event_count, 2u);
                    if (!filled || !fill_state.valid ||
                        fill_state.observed_event_count != expected_event_count ||
                        replay.scan_count() != 2u || replay.merge_pass_count() != 2u ||
                        replay.completed_scan_count() != 2u ||
                        replay.failed_scan_count() != 0u ||
                        replay.illegal_span_transition_count() != 0u ||
                        replay.visited_event_count() != expected_visited) {
                        throw gpu_index_direct_fill_invariant{};
                    }

                    std::uint64_t checksum = k_gpu_source_direct_checksum_seed;
                    std::uint64_t covered_positions = 0u;
                    for (std::size_t index = 0u; index < controls.size(); ++index) {
                        const gpu_minimizer_slot& control = controls[index];
                        const std::uint64_t interval_end =
                            index + 1u < controls.size()
                                ? controls[index + 1u].pos_offset
                                : positions.size();
                        const std::uint64_t expected_count =
                            interval_end - control.pos_offset;
                        if (control.pos_offset != covered_positions || expected_count == 0u ||
                            control.pos_count != expected_count ||
                            (index != 0u && controls[index - 1u].hash >= control.hash)) {
                            throw gpu_index_direct_fill_invariant{};
                        }
                        checksum = gpu_source_direct_checksum_word(checksum, control.hash);
                        checksum = gpu_source_direct_checksum_word(
                            checksum, control.pos_offset);
                        for (std::uint64_t position_index = control.pos_offset;
                             position_index < interval_end;
                             ++position_index) {
                            const std::uint64_t encoded =
                                positions[static_cast<std::size_t>(position_index)];
                            if ((encoded >> 1u) >= static_cast<std::uint64_t>(target_len) ||
                                (position_index != control.pos_offset &&
                                 positions[static_cast<std::size_t>(position_index - 1u)] >=
                                     encoded)) {
                                throw gpu_index_direct_fill_invariant{};
                            }
                            checksum = gpu_source_direct_checksum_word(checksum, encoded);
                        }
                        checksum = gpu_source_direct_checksum_word(checksum, expected_count);
                        covered_positions += expected_count;
                    }
                    if (covered_positions != positions.size() || checksum == 0u) {
                        throw gpu_index_direct_fill_invariant{};
                    }
                    stats.source_checksum = checksum;
                    stats.mapped_checksum = checksum;
                    stats.complete_source_traversals = 2u;
                    stats.complete_mapped_traversals = 1u;
                    return {
                        .actual_allocation_bytes = actual,
                        .fully_touched_bytes = actual,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected =
                            resident->slots.process_footprint_reflected &&
                            resident->positions.process_footprint_reflected,
                    };
                });
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
            .error = gpu_source_direct_materialization_error::non_host_visible_device_memory}};
    }

    stats.fully_touched_bytes =
        resident->materialization.allocation_snapshot.fully_touched_bytes;
    if (resident->materialization.is_permit_released) {
        stats.authority =
            gpu_resident_accounting_authority::reflected_process_footprint;
        stats.reflected_footprint_bytes = resident->materialization.reflected_delta_bytes;
    } else if (*resident_permit &&
               resident_permit->bytes() == stats.actual_allocation_bytes) {
        stats.authority = gpu_resident_accounting_authority::retained_exact_permit;
        stats.retained_permit_bytes = resident_permit->bytes();
        resident->permit = std::move(*resident_permit);
    } else {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    resident->accounting_authority = stats.authority;
    resident->target_len = target_len;
    resident->slot_mask = 0u;
    resident->position_count = static_cast<std::uint32_t>(expected_event_count);
    resident->bucket_count = static_cast<std::uint32_t>(distinct_count);
    resident->unique_key_count = distinct_count;
    resident->slot_capacity = distinct_count;
    resident->load_ppm = 1'000'000u;
    resident->max_probe = 0u;
    resident->representation = gpu_index_representation::source_direct_sorted_dense;
    resident->staging_bytes = 0u;
    resident->source_stream_checksum = stats.source_checksum;
    resident->mapped_stream_checksum = stats.mapped_checksum;
    resident->complete_source_traversals = stats.complete_source_traversals;
    resident->complete_mapped_traversals = stats.complete_mapped_traversals;
    resident->key = make_gpu_mapped_fragment_sorted_dense_key<p_minimizer>(
        runtime,
        operation_identity,
        target_len,
        distinct_count,
        expected_event_count,
        alphabet_size,
        kmer_size,
        window_size);
    resident->gate_identity = gpu_index_gate_identity_from_key(resident->key);
    const std::uint64_t allocation_sum = bioseq::common::saturating_add(
        resident->slots.allocation_bytes, resident->positions.allocation_bytes);
    if (!gpu_exact_resident_authority_valid(resident->accounting_authority,
                                            resident->materialization,
                                            static_cast<bool>(resident->permit),
                                            resident->permit.bytes(),
                                            allocation_sum)) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    return gpu_source_direct_materialization_result{
        .resident = std::move(resident),
        .stats = stats,
    };
}

namespace mapped_fragment_gpu_detail {

struct membership_digest {
    std::uint64_t xor_value = 0u;
    std::uint64_t sum_value = 0u;
    std::uint64_t count = 0u;

    void add(std::uint64_t hash, std::uint64_t encoded_position) noexcept {
        const std::uint64_t word = splitmix64(hash ^ splitmix64(encoded_position));
        xor_value ^= word;
        sum_value += word;
        ++count;
    }

    [[nodiscard]] bool operator==(const membership_digest&) const noexcept = default;
};

struct control_fill_state {
    std::span<gpu_minimizer_slot> controls{};
    std::uint64_t selected_hash_mask = 0u;
    std::uint64_t written = 0u;
    bool valid = true;
};

struct posting_fill_state {
    std::span<gpu_minimizer_slot> controls{};
    std::span<std::uint64_t> positions{};
    bioseq::storage::sequence_size_t target_len = 0u;
    std::uint64_t selected_hash_mask = 0u;
    membership_digest source_digest{};
    bool valid = true;
};

[[nodiscard]] inline bool accept_control(void* raw,
                                         std::uint64_t hash,
                                         std::uint64_t exact_count) noexcept {
    auto& state = *static_cast<control_fill_state*>(raw);
    if (!state.valid || exact_count == 0u ||
        exact_count > (std::numeric_limits<std::uint32_t>::max)() ||
        state.written >= state.controls.size() ||
        (hash & ~state.selected_hash_mask) != 0u) {
        state.valid = false;
        return false;
    }
    state.controls[static_cast<std::size_t>(state.written++)] = gpu_minimizer_slot{
        .hash = hash,
        .pos_offset = 0u,
        .pos_count = static_cast<std::uint32_t>(exact_count),
    };
    return true;
}

[[nodiscard]] inline bool accept_posting(void* raw,
                                         std::uint64_t hash,
                                         std::uint64_t encoded_position) noexcept {
    auto& state = *static_cast<posting_fill_state*>(raw);
    if (!state.valid || state.controls.empty() ||
        (encoded_position >> 1u) >= static_cast<std::uint64_t>(state.target_len) ||
        (hash & ~state.selected_hash_mask) != 0u) {
        state.valid = false;
        return false;
    }
    std::size_t lo = 0u;
    std::size_t hi = state.controls.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1u);
        if (state.controls[mid].hash < hash) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo >= state.controls.size() || state.controls[lo].hash != hash) {
        state.valid = false;
        return false;
    }
    gpu_minimizer_slot& control = state.controls[lo];
    const std::uint64_t expected_count =
        lo + 1u < state.controls.size()
            ? static_cast<std::uint64_t>(state.controls[lo + 1u].pos_offset) -
                  control.pos_offset
            : state.positions.size() - control.pos_offset;
    const std::uint64_t cursor = control.pos_count;
    if (expected_count == 0u || cursor >= expected_count ||
        control.pos_offset > state.positions.size() ||
        cursor > state.positions.size() - control.pos_offset) {
        state.valid = false;
        return false;
    }
    const std::size_t destination =
        static_cast<std::size_t>(control.pos_offset + cursor);
    if (destination >= state.positions.size()) {
        state.valid = false;
        return false;
    }
    state.positions[destination] = encoded_position;
    ++control.pos_count;
    state.source_digest.add(hash, encoded_position);
    return true;
}

template <class p_hash>
[[nodiscard]] inline std::size_t find_selected_hash(
    std::span<const p_hash> selected_hashes,
    std::uint64_t hash) noexcept {
    std::size_t lo = 0u;
    std::size_t hi = selected_hashes.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1u);
        if (static_cast<std::uint64_t>(selected_hashes[mid]) < hash) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

template <class p_hash>
struct query_union_count_state {
    std::span<const p_hash> selected_hashes{};
    std::span<std::uint64_t> occurrence_counts{};
    std::uint64_t selected_hash_mask = 0u;
    std::uint64_t selected_position_count = 0u;
    bool valid = true;
};

template <class p_hash>
[[nodiscard]] inline bool count_selected_control(
    void* raw,
    std::uint64_t hash,
    std::uint64_t exact_count) noexcept {
    auto& state = *static_cast<query_union_count_state<p_hash>*>(raw);
    if (!state.valid || exact_count == 0u ||
        (hash & ~state.selected_hash_mask) != 0u) {
        state.valid = false;
        return false;
    }
    const std::size_t index = find_selected_hash(state.selected_hashes, hash);
    if (index == state.selected_hashes.size() ||
        static_cast<std::uint64_t>(state.selected_hashes[index]) != hash) {
        return true;
    }
    if (exact_count > (std::numeric_limits<std::uint64_t>::max)() -
                          state.occurrence_counts[index] ||
        exact_count > (std::numeric_limits<std::uint64_t>::max)() -
                          state.selected_position_count) {
        state.valid = false;
        return false;
    }
    state.occurrence_counts[index] += exact_count;
    state.selected_position_count += exact_count;
    return true;
}

template <class p_hash>
struct query_union_posting_filter_state {
    std::span<const p_hash> selected_hashes{};
    void* destination_context = nullptr;
    prepared_minimizer_fragment_reader::complete_posting_visitor destination = nullptr;
    std::uint64_t selected_hash_mask = 0u;
    std::uint64_t expected_position_count = 0u;
    std::uint64_t forwarded_position_count = 0u;
    bool valid = true;
};

template <class p_hash>
[[nodiscard]] inline bool forward_selected_posting(
    void* raw,
    std::uint64_t hash,
    std::uint64_t encoded_position) noexcept {
    auto& state = *static_cast<query_union_posting_filter_state<p_hash>*>(raw);
    if (!state.valid || (hash & ~state.selected_hash_mask) != 0u) {
        state.valid = false;
        return false;
    }
    const std::size_t index = find_selected_hash(state.selected_hashes, hash);
    if (index == state.selected_hashes.size() ||
        static_cast<std::uint64_t>(state.selected_hashes[index]) != hash) {
        return true;
    }
    if (state.destination_context == nullptr || state.destination == nullptr ||
        state.forwarded_position_count >= state.expected_position_count ||
        !state.destination(state.destination_context, hash, encoded_position)) {
        state.valid = false;
        return false;
    }
    ++state.forwarded_position_count;
    return true;
}

/**
 * View of the stored index restricted to hashes requested by this, statement.
 *
 * Controls come from counts already accumulated across all fragments. Position
 * reads still scan the complete source, forwarding  only the selected hashes.
 * The source and the caller-owned hash and count arrays must outlive this view.
 */
template <class p_hash>
struct query_union_reader_view {
    const prepared_minimizer_fragment_reader* source = nullptr;
    std::span<const p_hash> selected_hashes{};
    std::span<const std::uint64_t> occurrence_counts{};
    gpu_source_direct_sorted_shape shape{};
    std::uint64_t selected_hash_mask = 0u;
    bool validated = false;

    [[nodiscard]] bool gpu_source_ready() const noexcept {
        return validated && source != nullptr && source->gpu_source_ready() &&
               !selected_hashes.empty() &&
               selected_hashes.size() == occurrence_counts.size() &&
               shape.minimizer_count != 0u && shape.distinct_count != 0u &&
               shape.minimizer_count <=
                   (std::numeric_limits<std::uint32_t>::max)() &&
               shape.distinct_count <=
                   (std::numeric_limits<std::uint32_t>::max)() &&
               shape.distinct_count <= selected_hashes.size() &&
               shape.distinct_count <= shape.minimizer_count &&
               shape.distinct_count <=
                   source->fragment_source_control_upper_bound() &&
               shape.minimizer_count <=
                   source->fragment_source_position_count();
    }

    [[nodiscard]] std::uint64_t
    fragment_source_control_upper_bound() const noexcept {
        return gpu_source_ready() ? shape.distinct_count : 0u;
    }

    [[nodiscard]] std::uint64_t fragment_source_position_count() const noexcept {
        return gpu_source_ready() ? shape.minimizer_count : 0u;
    }

    [[nodiscard]] std::uint32_t fragment_source_hash_bits() const noexcept {
        return gpu_source_ready() ? source->fragment_source_hash_bits() : 0u;
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_complete_control_groups(
        void* context,
        prepared_minimizer_fragment_reader::complete_control_group_visitor visitor) const {
        if (!gpu_source_ready() || context == nullptr || visitor == nullptr) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        std::uint64_t emitted_controls = 0u;
        std::uint64_t emitted_positions = 0u;
        for (std::size_t index = 0u; index < selected_hashes.size(); ++index) {
            const std::uint64_t count = occurrence_counts[index];
            if (count == 0u) {
                continue;
            }
            const std::uint64_t hash =
                static_cast<std::uint64_t>(selected_hashes[index]);
            if ((hash & ~selected_hash_mask) != 0u ||
                count > (std::numeric_limits<std::uint32_t>::max)() ||
                count > shape.minimizer_count ||
                emitted_positions > shape.minimizer_count - count ||
                !visitor(context, hash, count)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            emitted_positions += count;
            ++emitted_controls;
        }
        if (emitted_controls != shape.distinct_count ||
            emitted_positions != shape.minimizer_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::element_count);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_complete_postings(
        void* context,
        prepared_minimizer_fragment_reader::complete_posting_visitor visitor) const {
        if (!gpu_source_ready() || context == nullptr || visitor == nullptr) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        query_union_posting_filter_state<p_hash> state{
            .selected_hashes = selected_hashes,
            .destination_context = context,
            .destination = visitor,
            .selected_hash_mask = selected_hash_mask,
            .expected_position_count = shape.minimizer_count,
        };
        const auto visited = source->visit_complete_postings(
            std::addressof(state), &forward_selected_posting<p_hash>);
        if (!visited) {
            return std::unexpected(visited.error());
        }
        if (!state.valid ||
            state.forwarded_position_count != state.expected_position_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::element_count);
        }
        return {};
    }
};

} // namespace mapped_fragment_gpu_detail

/**
 * Count stored occurrences of the hashes requested by all queries in a statement.
 *
 * The caller supplies sorted unique hashes and zeroed counters. One scan of the
 * stored controls fills the counts without reading their position lists or
 * generating minimizers from the sequence.
 */
template <class p_hash>
[[nodiscard]] inline std::expected<
    gpu_source_direct_sorted_shape,
    gpu_source_direct_materialization_failure>
inspect_mapped_fragment_query_union(
    const prepared_minimizer_fragment_reader& reader,
    std::span<const p_hash> selected_hashes,
    std::span<std::uint64_t> occurrence_counts) {
    static_assert(std::same_as<p_hash, std::uint32_t> ||
                  std::same_as<p_hash, std::uint64_t>);
    const std::uint32_t source_hash_bits = reader.fragment_source_hash_bits();
    const std::uint64_t selected_hash_mask =
        source_hash_bits == 64u
            ? (std::numeric_limits<std::uint64_t>::max)()
            : source_hash_bits == 0u || source_hash_bits > 64u
                ? 0u
                : (std::uint64_t{1u} << source_hash_bits) - 1u;
    if (!reader.gpu_source_ready() || selected_hashes.empty() ||
        selected_hashes.size() != occurrence_counts.size() ||
        source_hash_bits == 0u || source_hash_bits > sizeof(p_hash) * 8u) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invalid_shape}};
    }
    for (std::size_t index = 0u; index < selected_hashes.size(); ++index) {
        const std::uint64_t hash =
            static_cast<std::uint64_t>(selected_hashes[index]);
        if (occurrence_counts[index] != 0u ||
            (hash & ~selected_hash_mask) != 0u ||
            (index != 0u && selected_hashes[index - 1u] >= selected_hashes[index])) {
            return std::unexpected{gpu_source_direct_materialization_failure{
                .error = gpu_source_direct_materialization_error::invalid_shape}};
        }
    }

    mapped_fragment_gpu_detail::query_union_count_state<p_hash> state{
        .selected_hashes = selected_hashes,
        .occurrence_counts = occurrence_counts,
        .selected_hash_mask = selected_hash_mask,
    };
    const auto counted = reader.visit_complete_control_groups(
        std::addressof(state),
        &mapped_fragment_gpu_detail::count_selected_control<p_hash>);
    if (!counted || !state.valid ||
        state.selected_position_count > reader.fragment_source_position_count()) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }

    std::uint64_t distinct_count = 0u;
    std::uint64_t verified_position_count = 0u;
    for (const std::uint64_t count : occurrence_counts) {
        if (count != 0u) {
            ++distinct_count;
        }
        if (count > (std::numeric_limits<std::uint32_t>::max)() ||
            count > (std::numeric_limits<std::uint64_t>::max)() -
                        verified_position_count) {
            return std::unexpected{gpu_source_direct_materialization_failure{
                .error = gpu_source_direct_materialization_error::device_abi_overflow}};
        }
        verified_position_count += count;
    }
    if (verified_position_count != state.selected_position_count) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    if (verified_position_count > (std::numeric_limits<std::uint32_t>::max)() ||
        distinct_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::device_abi_overflow}};
    }
    return gpu_source_direct_sorted_shape{
        .minimizer_count = verified_position_count,
        .distinct_count = distinct_count,
    };
}

/**
 * Build GPU lookup buffers from a validated stored minimizer reaher.
 *
 * Read the controls first, then sort and merge repeated hashes in the final
 * control  buffer. Their counts define contiguous position intervals. A second
 * pass fills those intervals directly from stored postings and checks that
 * their contents agree with the source.
 */
template <class p_minimizer, class p_reader>
[[nodiscard]] inline std::expected<
    gpu_source_direct_materialization_result,
    gpu_source_direct_materialization_failure>
materialize_sorted_dense_from_mapped_fragment_reader(
    bioseq::gpu::vulkan::runtime& runtime,
    const p_reader& reader,
    bioseq::storage::sequence_size_t target_len,
    std::uint64_t operation_identity,
    std::uint32_t alphabet_size,
    std::uint32_t kmer_size,
    std::uint32_t window_size) {
    using layout = typename p_minimizer::layout_type;
    const std::uint64_t control_upper_bound = reader.fragment_source_control_upper_bound();
    const std::uint64_t position_count = reader.fragment_source_position_count();
    const std::uint32_t source_hash_bits = reader.fragment_source_hash_bits();
    constexpr std::uint32_t selected_hash_bits =
        layout::k_storage_bits == 128u
            ? std::min<std::uint32_t>(layout::k_hash_bits, 56u)
            : layout::k_hash_bits;
    const bool device_uuid_bound = std::any_of(
        runtime.features().device_uuid.begin(),
        runtime.features().device_uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
    if constexpr (!std::is_standard_layout_v<p_minimizer> ||
                  !std::is_trivially_copyable_v<p_minimizer> ||
                  layout::k_storage_bits != sizeof(p_minimizer) * 8u ||
                  selected_hash_bits == 0u || selected_hash_bits > 64u ||
                  layout::k_position_bits == 0u || layout::k_position_bits > 63u) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::representation_ineligible}};
    }
    if (!runtime.is_usable() || !device_uuid_bound || !reader.gpu_source_ready() ||
        target_len == 0u || operation_identity == 0u || alphabet_size == 0u ||
        kmer_size == 0u || window_size == 0u || control_upper_bound == 0u ||
        position_count == 0u || control_upper_bound > position_count ||
        source_hash_bits == 0u || source_hash_bits > selected_hash_bits ||
        !gpu_source_direct_tuple_supported<p_minimizer>(
            alphabet_size, kmer_size, window_size) ||
        source_hash_bits != 2u * kmer_size ||
        control_upper_bound > (std::numeric_limits<std::uint32_t>::max)() ||
        position_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invalid_shape}};
    }
    auto preflight = plan_gpu_source_direct_sorted_dense(
        runtime, position_count, control_upper_bound);
    if (!preflight) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = preflight.error()}};
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto resident_permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        preflight->conservative_request_bytes, &admission_failure);
    if (!resident_permit) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::resident_memory_admission_denied,
            .admission = admission_failure}};
    }

    auto resident = std::make_shared<gpu_resident_minimizer_index>();
    gpu_source_direct_materialization_stats stats{
        .shape = {.minimizer_count = position_count, .distinct_count = control_upper_bound},
        .logical_slot_bytes = preflight->control_bytes,
        .logical_position_bytes = preflight->position_bytes,
        .logical_resident_bytes = preflight->logical_resident_bytes,
        .control_allocation_requirement_bytes =
            preflight->control_allocation_requirement_bytes,
        .position_allocation_requirement_bytes =
            preflight->position_allocation_requirement_bytes,
        .control_allocation_allowance_bytes =
            preflight->control_allocation_allowance_bytes,
        .position_allocation_allowance_bytes =
            preflight->position_allocation_allowance_bytes,
        .conservative_allowance_bytes = preflight->allowance_bytes,
        .conservative_request_bytes = preflight->conservative_request_bytes,
    };
    bool allocation_requirement_mismatch = false;
    bool non_host_visible = false;
    auto* const runtime_ptr = std::addressof(runtime);
    try {
        resident->materialization =
            bioseq::parallelism::memory_admission_controller::instance().materialize(
                *resident_permit,
                [resident,
                 &reader,
                 target_len,
                 control_upper_bound,
                 position_count,
                 selected_hash_mask = source_hash_bits == 64u
                     ? (std::numeric_limits<std::uint64_t>::max)()
                     : (std::uint64_t{1u} << source_hash_bits) - 1u,
                 preflight = *preflight,
                 runtime_ptr,
                 &stats,
                 &allocation_requirement_mismatch,
                 &non_host_visible]() -> bioseq::parallelism::memory_materialization_snapshot {
                    // CPU prefix searches also read these buffers. Use cached host memory
                    // that remains accessible to Vulkan.
                resident->slots = runtime_ptr->create_host_buffer(
                        preflight.control_bytes, k_gpu_source_direct_buffer_usage);
                    resident->positions = runtime_ptr->create_host_buffer(
                        preflight.position_bytes, k_gpu_source_direct_buffer_usage);
                    if (resident->slots.allocation_bytes !=
                            preflight.control_allocation_requirement_bytes ||
                        resident->positions.allocation_bytes !=
                            preflight.position_allocation_requirement_bytes) {
                        allocation_requirement_mismatch = true;
                        resident->positions.reset();
                        resident->slots.reset();
                        return {};
                    }
                    const std::uint64_t actual = bioseq::common::saturating_add(
                        resident->slots.allocation_bytes, resident->positions.allocation_bytes);
                    if (actual != preflight.allocation_requirement_bytes) {
                        allocation_requirement_mismatch = true;
                        resident->positions.reset();
                        resident->slots.reset();
                        return {};
                    }
                    stats.actual_slot_allocation_bytes = resident->slots.allocation_bytes;
                    stats.actual_position_allocation_bytes = resident->positions.allocation_bytes;
                    stats.actual_allocation_bytes = actual;
                    ++stats.permit_reconciliations;
                    if (resident->slots.mapped == nullptr || resident->positions.mapped == nullptr ||
                        !resident->slots.is_host_visible_coherent() ||
                        !resident->positions.is_host_visible_coherent()) {
                        non_host_visible = true;
                        return {
                            .actual_allocation_bytes = actual,
                            .is_host_visible = false,
                            .is_host_coherent = false,
                            .is_process_footprint_reflected = false,
                        };
                    }
                    auto controls = std::span<gpu_minimizer_slot>{
                        static_cast<gpu_minimizer_slot*>(resident->slots.mapped),
                        static_cast<std::size_t>(control_upper_bound)};
                    auto positions = std::span<std::uint64_t>{
                        static_cast<std::uint64_t*>(resident->positions.mapped),
                        static_cast<std::size_t>(position_count)};
                    std::memset(resident->slots.mapped,
                                0,
                                static_cast<std::size_t>(resident->slots.allocation_bytes));
                    std::memset(resident->positions.mapped,
                                0,
                                static_cast<std::size_t>(resident->positions.allocation_bytes));

                    mapped_fragment_gpu_detail::control_fill_state control_state{
                        .controls = controls,
                        .selected_hash_mask = selected_hash_mask};
                    const auto controls_read = reader.visit_complete_control_groups(
                        std::addressof(control_state),
                        &mapped_fragment_gpu_detail::accept_control);
                    if (!controls_read || !control_state.valid ||
                        control_state.written != control_upper_bound) {
                        throw gpu_index_direct_fill_invariant{};
                    }
                    bioseq::sorting::radix_sort(
                        controls.data(),
                        controls.data() + controls.size(),
                        [](const gpu_minimizer_slot& slot) noexcept { return slot.hash; });

                    std::size_t distinct_count = 0u;
                    std::uint64_t covered_positions = 0u;
                    for (std::size_t source = 0u; source < controls.size();) {
                        const std::uint64_t hash = controls[source].hash;
                        std::uint64_t exact_count = 0u;
                        do {
                            if (controls[source].pos_count == 0u ||
                                exact_count > (std::numeric_limits<std::uint32_t>::max)() -
                                                  controls[source].pos_count) {
                                throw gpu_index_direct_fill_invariant{};
                            }
                            exact_count += controls[source].pos_count;
                            ++source;
                        } while (source < controls.size() && controls[source].hash == hash);
                        if (covered_positions >
                                (std::numeric_limits<std::uint32_t>::max)() ||
                            exact_count == 0u || exact_count > position_count ||
                            covered_positions > position_count - exact_count) {
                            throw gpu_index_direct_fill_invariant{};
                        }
                        controls[distinct_count++] = gpu_minimizer_slot{
                            .hash = hash,
                            .pos_offset = static_cast<std::uint32_t>(covered_positions),
                            .pos_count = static_cast<std::uint32_t>(exact_count),
                        };
                        covered_positions += exact_count;
                    }
                    if (distinct_count == 0u || covered_positions != position_count ||
                        distinct_count > (std::numeric_limits<std::uint32_t>::max)()) {
                        throw gpu_index_direct_fill_invariant{};
                    }
                    auto compact_controls = controls.first(distinct_count);
                    for (gpu_minimizer_slot& control : compact_controls) {
                        control.pos_count = 0u;
                    }
                    mapped_fragment_gpu_detail::posting_fill_state posting_state{
                        .controls = compact_controls,
                        .positions = positions,
                        .target_len = target_len,
                        .selected_hash_mask = selected_hash_mask,
                    };
                    const auto postings_read = reader.visit_complete_postings(
                        std::addressof(posting_state),
                        &mapped_fragment_gpu_detail::accept_posting);
                    if (!postings_read || !posting_state.valid ||
                        posting_state.source_digest.count != position_count) {
                        throw gpu_index_direct_fill_invariant{};
                    }

                    mapped_fragment_gpu_detail::membership_digest mapped_digest{};
                    std::uint64_t checksum = k_gpu_source_direct_checksum_seed;
                    covered_positions = 0u;
                    for (std::size_t index = 0u; index < compact_controls.size(); ++index) {
                        gpu_minimizer_slot& control = compact_controls[index];
                        const std::uint64_t expected_count =
                            index + 1u < compact_controls.size()
                                ? static_cast<std::uint64_t>(
                                      compact_controls[index + 1u].pos_offset) -
                                      control.pos_offset
                                : position_count - control.pos_offset;
                        if (control.pos_count != expected_count || expected_count == 0u ||
                            control.pos_offset != covered_positions ||
                            (index != 0u &&
                             compact_controls[index - 1u].hash >= control.hash)) {
                            throw gpu_index_direct_fill_invariant{};
                        }
                        checksum = gpu_source_direct_checksum_word(checksum, control.hash);
                        checksum = gpu_source_direct_checksum_word(checksum, control.pos_offset);
                        for (std::uint64_t offset = control.pos_offset;
                             offset < control.pos_offset + expected_count;
                             ++offset) {
                            const std::uint64_t encoded = positions[static_cast<std::size_t>(offset)];
                            if ((encoded >> 1u) >= static_cast<std::uint64_t>(target_len) ||
                                (offset != control.pos_offset &&
                                 positions[static_cast<std::size_t>(offset - 1u)] >= encoded)) {
                                throw gpu_index_direct_fill_invariant{};
                            }
                            mapped_digest.add(control.hash, encoded);
                            checksum = gpu_source_direct_checksum_word(checksum, encoded);
                        }
                        checksum = gpu_source_direct_checksum_word(checksum, expected_count);
                        covered_positions += expected_count;
                    }
                    if (covered_positions != position_count ||
                        !(posting_state.source_digest == mapped_digest) || checksum == 0u) {
                        throw gpu_index_direct_fill_invariant{};
                    }
                    const std::uint64_t logical_slot_bytes = bioseq::common::saturating_mul(
                        distinct_count, sizeof(gpu_minimizer_slot));
                    const std::uint64_t logical_position_bytes = bioseq::common::saturating_mul(
                        position_count, sizeof(std::uint64_t));
                    const std::uint64_t logical_resident_bytes = bioseq::common::saturating_add(
                        logical_slot_bytes, logical_position_bytes);
                    if (logical_slot_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                        logical_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                        logical_resident_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                        throw gpu_index_direct_fill_invariant{};
                    }
                    stats.shape.distinct_count = distinct_count;
                    stats.logical_slot_bytes = logical_slot_bytes;
                    stats.logical_position_bytes = logical_position_bytes;
                    stats.logical_resident_bytes = logical_resident_bytes;
                    stats.control_allocation_allowance_bytes =
                        stats.actual_slot_allocation_bytes - logical_slot_bytes;
                    stats.position_allocation_allowance_bytes =
                        stats.actual_position_allocation_bytes - logical_position_bytes;
                    stats.conservative_allowance_bytes = bioseq::common::saturating_add(
                        stats.control_allocation_allowance_bytes,
                        stats.position_allocation_allowance_bytes);
                    stats.source_checksum = checksum;
                    stats.mapped_checksum = checksum;
                    stats.complete_source_traversals = 2u;
                    stats.complete_mapped_traversals = 1u;
                    return {
                        .actual_allocation_bytes = actual,
                        .fully_touched_bytes = actual,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected =
                            resident->slots.process_footprint_reflected &&
                            resident->positions.process_footprint_reflected,
                    };
                });
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
            .error = gpu_source_direct_materialization_error::non_host_visible_device_memory}};
    }

    stats.fully_touched_bytes =
        resident->materialization.allocation_snapshot.fully_touched_bytes;
    if (resident->materialization.is_permit_released) {
        stats.authority = gpu_resident_accounting_authority::reflected_process_footprint;
        stats.reflected_footprint_bytes = resident->materialization.reflected_delta_bytes;
    } else if (*resident_permit &&
               resident_permit->bytes() == stats.actual_allocation_bytes) {
        stats.authority = gpu_resident_accounting_authority::retained_exact_permit;
        stats.retained_permit_bytes = resident_permit->bytes();
        resident->permit = std::move(*resident_permit);
    } else {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    resident->accounting_authority = stats.authority;
    resident->target_len = target_len;
    resident->slot_mask = 0u;
    resident->position_count = static_cast<std::uint32_t>(position_count);
    resident->bucket_count = static_cast<std::uint32_t>(stats.shape.distinct_count);
    resident->unique_key_count = stats.shape.distinct_count;
    resident->slot_capacity = stats.shape.distinct_count;
    resident->load_ppm = 1'000'000u;
    resident->max_probe = 0u;
    resident->representation = gpu_index_representation::source_direct_sorted_dense;
    resident->staging_bytes = 0u;
    resident->source_stream_checksum = stats.source_checksum;
    resident->mapped_stream_checksum = stats.mapped_checksum;
    resident->complete_source_traversals = stats.complete_source_traversals;
    resident->complete_mapped_traversals = stats.complete_mapped_traversals;
    resident->key = make_gpu_mapped_fragment_sorted_dense_key<p_minimizer>(
        runtime,
        operation_identity,
        target_len,
        stats.shape.distinct_count,
        position_count,
        alphabet_size,
        kmer_size,
        window_size);
    resident->gate_identity = gpu_index_gate_identity_from_key(resident->key);
    const std::uint64_t allocation_sum = bioseq::common::saturating_add(
        resident->slots.allocation_bytes, resident->positions.allocation_bytes);
    if (!gpu_exact_resident_authority_valid(resident->accounting_authority,
                                            resident->materialization,
                                            static_cast<bool>(resident->permit),
                                            resident->permit.bytes(),
                                            allocation_sum)) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invariant_failed}};
    }
    return gpu_source_direct_materialization_result{
        .resident = std::move(resident),
        .stats = stats,
    };
}

/** Build GPU lookup buffers for the stored hashes selected by this statement */
template <class p_minimizer, class p_hash>
[[nodiscard]] inline std::expected<
    gpu_source_direct_materialization_result,
    gpu_source_direct_materialization_failure>
materialize_sorted_dense_from_mapped_fragment_query_union(
    bioseq::gpu::vulkan::runtime& runtime,
    const prepared_minimizer_fragment_reader& reader,
    std::span<const p_hash> selected_hashes,
    std::span<const std::uint64_t> occurrence_counts,
    gpu_source_direct_sorted_shape selected_shape,
    bioseq::storage::sequence_size_t target_len,
    std::uint64_t operation_identity,
    std::uint32_t alphabet_size,
    std::uint32_t kmer_size,
    std::uint32_t window_size) {
    static_assert(std::same_as<p_hash, std::uint32_t> ||
                  std::same_as<p_hash, std::uint64_t>);
    const std::uint32_t source_hash_bits = reader.fragment_source_hash_bits();
    const std::uint64_t selected_hash_mask =
        source_hash_bits == 64u
            ? (std::numeric_limits<std::uint64_t>::max)()
            : source_hash_bits == 0u || source_hash_bits > 64u
                ? 0u
                : (std::uint64_t{1u} << source_hash_bits) - 1u;
    std::uint64_t verified_positions = 0u;
    std::uint64_t verified_distinct = 0u;
    bool valid = reader.gpu_source_ready() && !selected_hashes.empty() &&
                 selected_hashes.size() == occurrence_counts.size() &&
                 selected_shape.minimizer_count != 0u &&
                 selected_shape.distinct_count != 0u &&
                 selected_shape.distinct_count <=
                     reader.fragment_source_control_upper_bound() &&
                 selected_shape.minimizer_count <=
                     reader.fragment_source_position_count() &&
                 source_hash_bits != 0u && source_hash_bits <= sizeof(p_hash) * 8u;
    for (std::size_t index = 0u; valid && index < selected_hashes.size(); ++index) {
        const std::uint64_t hash =
            static_cast<std::uint64_t>(selected_hashes[index]);
        const std::uint64_t count = occurrence_counts[index];
        valid = (hash & ~selected_hash_mask) == 0u &&
                (index == 0u || selected_hashes[index - 1u] < selected_hashes[index]) &&
                count <= (std::numeric_limits<std::uint32_t>::max)() &&
                verified_positions <= selected_shape.minimizer_count &&
                count <= selected_shape.minimizer_count - verified_positions;
        if (valid) {
            verified_positions += count;
            verified_distinct += count != 0u ? 1u : 0u;
        }
    }
    if (!valid || verified_positions != selected_shape.minimizer_count ||
        verified_distinct != selected_shape.distinct_count) {
        return std::unexpected{gpu_source_direct_materialization_failure{
            .error = gpu_source_direct_materialization_error::invalid_shape}};
    }
    const mapped_fragment_gpu_detail::query_union_reader_view<p_hash> selected_reader{
        .source = std::addressof(reader),
        .selected_hashes = selected_hashes,
        .occurrence_counts = occurrence_counts,
        .shape = selected_shape,
        .selected_hash_mask = selected_hash_mask,
        .validated = true,
    };
    auto materialized =
        materialize_sorted_dense_from_mapped_fragment_reader<p_minimizer>(
            runtime,
            selected_reader,
            target_len,
            operation_identity,
            alphabet_size,
            kmer_size,
            window_size);
    if (!materialized || !materialized->resident ||
        materialized->stats.shape.minimizer_count != selected_shape.minimizer_count ||
        materialized->stats.shape.distinct_count != selected_shape.distinct_count) {
        return materialized
            ? std::unexpected{gpu_source_direct_materialization_failure{
                  .error = gpu_source_direct_materialization_error::invariant_failed}}
            : std::unexpected{materialized.error()};
    }
    return materialized;
}

/// Failures that prevent ownership transfer from a runtime-built GPU index.
enum class gpu_built_index_adoption_error : std::uint8_t {
    invalid_build,
    missing_ownership,
    stats_mismatch,
};

/// Return the stable diagnostic name for a built-index adoption failure.
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

/// Transfer a runtime-built index into a resident owner for reuse.
/// Reuse its device buffers and memory permit. If the permit was released,
/// require a  record showing that the process footprint covers the allocation.
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
    resident->accounting_authority = retained
        ? gpu_resident_accounting_authority::retained_exact_permit
        : gpu_resident_accounting_authority::reflected_process_footprint;
    resident->ownership = built.ownership;
    resident->ownership.resident_transfers = 1u;
    resident->ownership.adoption_successes = 1u;
    resident->key = key;
    resident->target_len = built.target_len;
    resident->slot_mask = built.slot_mask;          // Sorted controls do not use a hash-table mask.
    resident->position_count = built.position_count;
    resident->bucket_count = built.occupied_slots;  // Number of sorted controls searched by hash.
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

/**
 * Prepare an adopted GPU-built index for CPU searches of its mapped buffers.
 *
 * Check the build record, then read each final control and posxtion once to
 * verify sorted hashes, contiguous intervals, and, target bounds. Save the
 * resulting checksum for the prefix-search consumer. The existing buffers
 * remain the index used by both CPU and GPU code.
 */
[[nodiscard]] inline bool authenticate_gpu_built_sorted_dense_prefix(
    const bioseq::gpu::vulkan::runtime& runtime,
    gpu_resident_minimizer_index& resident) noexcept {
    const auto& key = resident.key;
    if (!runtime.is_usable() || !resident.slots.is_valid() ||
        !resident.positions.is_valid() || resident.slots.owner != &runtime ||
        resident.positions.owner != &runtime || resident.slots.mapped == nullptr ||
        resident.positions.mapped == nullptr ||
        !resident.slots.is_host_visible_coherent() ||
        !resident.positions.is_host_visible_coherent() ||
        resident.representation != gpu_index_representation::source_direct_sorted_dense ||
        !key.source_direct_sorted_dense ||
        key.representation != gpu_index_representation::source_direct_sorted_dense ||
        key.sorted_dense_abi_revision != k_minimizer_sorted_dense_gpu_abi_revision ||
        key.target_len != static_cast<std::uint64_t>(resident.target_len) ||
        key.table_size != resident.bucket_count ||
        key.position_count != resident.position_count || resident.bucket_count == 0u ||
        resident.position_count == 0u || resident.bucket_count > resident.position_count ||
        resident.slot_mask != 0u || resident.target_position_offset != 0u ||
        resident.unique_key_count != resident.bucket_count ||
        resident.slot_capacity != resident.bucket_count || resident.gate_identity.enabled ||
        key.gate_enabled || key.gate_word_count != 0u || key.gate_mask != 0u ||
        key.gate_digest_lo != 0u || key.gate_digest_hi != 0u ||
        key.gate_canonical_words || key.device_uuid != runtime.features().device_uuid ||
        resident.ownership.count_emit_parity_verified != 1u ||
        resident.ownership.counted_minimizer_count != resident.position_count ||
        resident.ownership.verified_emit_count != resident.position_count ||
        resident.ownership.resident_transfers != 1u ||
        resident.ownership.adoption_successes != 1u ||
        resident.source_stream_checksum != 0u || resident.mapped_stream_checksum != 0u ||
        resident.complete_source_traversals != 0u ||
        resident.complete_mapped_traversals != 0u) {
        return false;
    }
    const std::uint64_t control_bytes = bioseq::common::saturating_mul(
        resident.bucket_count, sizeof(gpu_minimizer_slot));
    const std::uint64_t position_bytes = bioseq::common::saturating_mul(
        resident.position_count, sizeof(std::uint64_t));
    if (control_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        control_bytes > resident.slots.bytes || position_bytes > resident.positions.bytes) {
        return false;
    }
    const auto controls = std::span<const gpu_minimizer_slot>{
        static_cast<const gpu_minimizer_slot*>(resident.slots.mapped),
        resident.bucket_count};
    const auto positions = std::span<const std::uint64_t>{
        static_cast<const std::uint64_t*>(resident.positions.mapped),
        resident.position_count};
    std::uint64_t checksum = k_gpu_source_direct_checksum_seed;
    std::uint64_t covered_positions = 0u;
    std::uint64_t previous_hash = 0u;
    bool have_previous_hash = false;
    for (const gpu_minimizer_slot& control : controls) {
        const std::uint64_t offset = control.pos_offset;
        const std::uint64_t count = control.pos_count;
        if (count == 0u || offset != covered_positions || offset > positions.size() ||
            count > positions.size() - offset ||
            (have_previous_hash && control.hash <= previous_hash)) {
            return false;
        }
        checksum = gpu_source_direct_checksum_word(checksum, control.hash);
        checksum = gpu_source_direct_checksum_word(checksum, offset);
        for (std::uint64_t index = offset; index < offset + count; ++index) {
            const std::uint64_t encoded = positions[static_cast<std::size_t>(index)];
            if ((encoded >> 1u) >= static_cast<std::uint64_t>(resident.target_len)) {
                return false;
            }
            checksum = gpu_source_direct_checksum_word(checksum, encoded);
        }
        checksum = gpu_source_direct_checksum_word(checksum, count);
        covered_positions += count;
        previous_hash = control.hash;
        have_previous_hash = true;
    }
    if (covered_positions != positions.size() || checksum == 0u) {
        return false;
    }
    resident.source_stream_checksum = checksum;
    resident.mapped_stream_checksum = checksum;
    resident.complete_source_traversals = 1u;
    resident.complete_mapped_traversals = 1u;
    return true;
}

} // namespace bioseq::indexing
