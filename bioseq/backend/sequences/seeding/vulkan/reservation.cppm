/**
 * @file reservation.cppm
 * @brief Exact host, device, and retained-output seed reservations.
 *
 * Reservation plans combine driver-reported Vulkan allocation requirements
 * with host prefix/result storage and caller-retained anchor growth. Every sum
 * saturates so an overflowed plan is rejected before memory admission.
 */
module;

#include <fmt/format.h>
#include <libassert/assert.hpp>

#include <cstring>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <vulkan/vulkan_core.h>

export module bioseq.seeding.vulkan:reservation;

export import :diagnostics;
import bioseq.common.checked_arithmetic;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::seeding {

// ===========================================================================
// Memory reservation
// ===========================================================================

/** @brief Exact host, device, and retained-output reservation for one lookup. */
struct vulkan_seed_reservation_plan {
    /** Saturated sum of every reservation component. */
    std::uint64_t total_bytes = 0u;
    /** Driver-accounted device allocation requirements. */
    std::uint64_t vulkan_allocation_requirement_bytes = 0u;
    /** Host bytes retaining the exclusive prefix array. */
    std::uint64_t host_prefix_bytes = 0u;
    /** Host bytes receiving the complete lookup result. */
    std::uint64_t host_result_bytes = 0u;
    /** Additional bytes retained when the caller's anchor slab must grow. */
    std::uint64_t anchors_out_growth_bytes = 0u;
    /** Whether no component sum saturated at `std::uint64_t` maximum. */
    bool is_arithmetic_valid = false;
};

/** @brief Driver-accounted Vulkan buffer allocation requirements by purpose. */
struct vulkan_seed_buffer_allocation_requirements {
    /** Shared count/scan scratch buffer requirement. */
    std::uint64_t scratch_bytes = 0u;
    /** Uploaded minimizer buffer requirement, or zero for device-resident input. */
    std::uint64_t uploaded_minimizer_bytes = 0u;
    /** Exclusive-prefix buffer requirement. */
    std::uint64_t prefix_bytes = 0u;
    /** Anchor payload buffer requirement. */
    std::uint64_t anchors_bytes = 0u;
    /** Anchor-sort key buffer requirement. */
    std::uint64_t anchor_keys_bytes = 0u;
    /** Bitonic sort-range buffer requirement. */
    std::uint64_t sort_ranges_bytes = 0u;
    /** Radix-sort anchor scratch requirement. */
    std::uint64_t anchors_scratch_bytes = 0u;
    /** Radix-sort key scratch requirement. */
    std::uint64_t anchor_keys_scratch_bytes = 0u;
    /** Radix histogram buffer requirement. */
    std::uint64_t histogram_bytes = 0u;

    /** @return Saturated sum of all driver-accounted buffer requirements. */
    [[nodiscard]] constexpr std::uint64_t total_bytes() const noexcept {
        std::uint64_t value = 0u;
        value = bioseq::common::saturating_add(value, scratch_bytes);
        value = bioseq::common::saturating_add(
            value, uploaded_minimizer_bytes);
        value = bioseq::common::saturating_add(value, prefix_bytes);
        value = bioseq::common::saturating_add(value, anchors_bytes);
        value = bioseq::common::saturating_add(value, anchor_keys_bytes);
        value = bioseq::common::saturating_add(value, sort_ranges_bytes);
        value = bioseq::common::saturating_add(value, anchors_scratch_bytes);
        value = bioseq::common::saturating_add(
            value, anchor_keys_scratch_bytes);
        return bioseq::common::saturating_add(value, histogram_bytes);
    }
};

/**
 * @brief Compose a complete lookup reservation from device and host requirements.
 * @param requirements Driver-accounted Vulkan buffer requirements by purpose.
 * @param host_prefix_bytes Host bytes retaining the exclusive prefix array.
 * @param host_result_bytes Host bytes receiving downloaded lookup results.
 * @param anchors_out_growth_bytes Additional caller-retained anchor capacity.
 * @return Component-preserving plan whose validity is false after saturation.
 */
[[nodiscard]] inline constexpr vulkan_seed_reservation_plan
make_vulkan_seed_reservation_plan(
    const vulkan_seed_buffer_allocation_requirements& requirements,
    std::uint64_t host_prefix_bytes,
    std::uint64_t host_result_bytes,
    std::uint64_t anchors_out_growth_bytes) noexcept
{
    constexpr std::uint64_t k_maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t vulkan_bytes = requirements.total_bytes();
    std::uint64_t total = vulkan_bytes;
    total = bioseq::common::saturating_add(total, host_prefix_bytes);
    total = bioseq::common::saturating_add(total, host_result_bytes);
    total = bioseq::common::saturating_add(total, anchors_out_growth_bytes);
    return vulkan_seed_reservation_plan{
        .total_bytes = total,
        .vulkan_allocation_requirement_bytes = vulkan_bytes,
        .host_prefix_bytes = host_prefix_bytes,
        .host_result_bytes = host_result_bytes,
        .anchors_out_growth_bytes = anchors_out_growth_bytes,
        .is_arithmetic_valid =
            vulkan_bytes != k_maximum && total != k_maximum,
    };
}

/**
 * @brief Report whether a reservation is valid and fits an allocation budget.
 * @param plan Complete host, device, and retained-output reservation.
 * @param allocatable_bytes Bytes available after existing reservations.
 * @return `true` when arithmetic stayed finite and the total fits the budget.
 */
[[nodiscard]] inline constexpr bool vulkan_seed_reservation_fits(
    const vulkan_seed_reservation_plan& plan,
    std::uint64_t allocatable_bytes) noexcept
{
    return plan.is_arithmetic_valid && plan.total_bytes <= allocatable_bytes;
}

/** Storage and bidirectional-transfer usage shared by seed-lookup buffers. */
inline constexpr VkBufferUsageFlags k_vulkan_seed_buffer_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

/**
 * @brief Plan a prefix-driven Vulkan seed lookup.
 * @tparam p_minimizer_type Minimizer record type and device wire representation.
 * @param vulkan_runtime Shared runtime used to query driver allocation requirements.
 * @param minimizer_count Query minimizers consumed by the lookup.
 * @param sort_plan Validated anchor-count and selected-sort geometry.
 * @param has_device_minimizers Whether query minimizers already reside on the device.
 * @param anchors_out_capacity Existing retained anchor capacity, in anchor records.
 * @return Exact component plan, marked invalid when any size arithmetic saturates.
 *
 * The driver is queried independently for every nonempty buffer because its
 * allocation requirement can exceed the logical payload byte count.
 */
template <class p_minimizer_type>
[[nodiscard]] vulkan_seed_reservation_plan plan_vulkan_seed_lookup_reservation(
    bioseq::gpu::vulkan::runtime& vulkan_runtime,
    std::uint64_t minimizer_count,
    const bioseq::gpu::vulkan::seed_anchor_sort_plan& sort_plan,
    bool has_device_minimizers,
    std::uint64_t anchors_out_capacity)
{
    using bioseq::common::saturating_mul;
    const std::uint64_t min_buffer_bytes = has_device_minimizers
        ? 0u
        : std::max<std::uint64_t>(
            bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::device_bytes_for_count(
                minimizer_count),
            4u);
    const std::uint64_t prefix_count =
        bioseq::common::saturating_add(minimizer_count, 1u);
    const std::uint64_t host_prefix_bytes =
        saturating_mul(prefix_count, sizeof(std::uint32_t));
    const std::uint64_t prefix_buffer_bytes =
        std::max<std::uint64_t>(host_prefix_bytes, 4u);
    const std::uint64_t anchors_out_growth =
        anchors_out_capacity < sort_plan.anchor_count
            ? sort_plan.anchor_bytes : 0u;
    const auto requirement = [&vulkan_runtime](std::uint64_t bytes) {
        return bytes == 0u ? 0u : vulkan_runtime.buffer_allocation_requirement(
            bytes, k_vulkan_seed_buffer_usage);
    };
    vulkan_seed_buffer_allocation_requirements requirements{
        .scratch_bytes = requirement(4u),
        .uploaded_minimizer_bytes = requirement(min_buffer_bytes),
        .prefix_bytes = requirement(prefix_buffer_bytes),
        .anchors_bytes = requirement(sort_plan.anchor_bytes),
        .anchor_keys_bytes = requirement(sort_plan.key_bytes),
    };
    if (sort_plan.backend == bioseq::gpu::vulkan::seed_anchor_sort_backend::bitonic) {
        requirements.sort_ranges_bytes = requirement(
            sizeof(bioseq::gpu::vulkan::sort_range_wire));
    } else if (sort_plan.backend == bioseq::gpu::vulkan::seed_anchor_sort_backend::radix) {
        requirements.anchors_scratch_bytes =
            requirement(sort_plan.anchor_bytes);
        requirements.anchor_keys_scratch_bytes =
            requirement(sort_plan.key_bytes);
        requirements.histogram_bytes = requirement(sort_plan.histogram_bytes);
    }
    return make_vulkan_seed_reservation_plan(
        requirements,
        host_prefix_bytes,
        sort_plan.host_result_bytes,
        anchors_out_growth);
}

/**
 * @brief Compute the saturated transient reservation for fused GPU seed counting.
 * @tparam p_minimizer_type Minimizer record type and device wire representation.
 * @param minimizer_count Query minimizers consumed by the fused lookup.
 * @param has_device_minimizers Whether query minimizers already reside on the device.
 * @return Saturated bytes for count, prefix, scan-block, query-record,
 * optional minimizer upload, and arena-alignment storage.
 */
template <class p_minimizer_type>
[[nodiscard]] std::uint64_t vulkan_fused_seed_lookup_reservation_bytes(
    std::uint64_t minimizer_count,
    bool has_device_minimizers) noexcept
{
    using bioseq::common::saturating_mul;
    const std::uint64_t min_bytes = has_device_minimizers
        ? 0u
        : bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::device_bytes_for_count(minimizer_count);
    constexpr std::uint64_t k_scan_block = 256u;
    constexpr std::uint64_t k_arena_alignment_slack = 6u * 256u;
    const std::uint64_t num_blocks = minimizer_count == 0u
        ? 1u
        : (minimizer_count + k_scan_block - 1u) / k_scan_block;
    std::uint64_t total = 0;
    total = bioseq::common::saturating_add(
        total, saturating_mul(minimizer_count, sizeof(std::uint32_t)));  // counts
    total = bioseq::common::saturating_add(
        total, saturating_mul(minimizer_count, sizeof(std::uint32_t)));  // prefix
    total = bioseq::common::saturating_add(
        total, saturating_mul(num_blocks, sizeof(std::uint32_t)));       // scan blocks
    total = bioseq::common::saturating_add(total, 16u); // Query record.
    total = bioseq::common::saturating_add(total, min_bytes);
    total = bioseq::common::saturating_add(total, k_arena_alignment_slack);
    return total;
}


}  // namespace bioseq::seeding
