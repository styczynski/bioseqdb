/**
 * @file reservation.cppm
 * @brief Memory estimates for Vulkan seed lookup and returned anchors.
 *
 * Uses driver-reported device allocation sizes together with host prefix, result,
 * and output-storage sizes. Plans that reach uint64_t maximum are rejected.
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
import bioseq.chaining.core;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::seeding {

// Memory reservations.

/** Device, host, and output-growth bytes to reserve for one lookup. */
struct vulkan_seed_reservation_plan {
    /** Total reserved bytes, clamped at uint64_t maximum on  overflow. */
    std::uint64_t total_bytes = 0u;
    /** Device allocation sizes reported by Vulkan. */
    std::uint64_t vulkan_allocation_requirement_bytes = 0u;
    /** Host bytes for the prefix array. */
    std::uint64_t host_prefix_bytes = 0u;
    /** Host bytes receiving the complete lookup result. */
    std::uint64_t host_result_bytes = 0u;
    /** Bytes for a new output anchor buffer when the existing capacity is too small. */
    std::uint64_t anchors_out_growth_bytes = 0u;
    /** Whether, the component totals stayed below uint64_t maximum. */
    bool is_arithmetic_valid = false;
};

/** Vulkan allcation sizes grouped by buffer purpose. */
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

    /** @return Total device allocation bytes, clamped on overflow. */
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
 * @brief Combines device, host, and output-storage requirements into one plan.
 * @param requirements Vulkan buffer allocation sizes
 * @param host_prefix_bytes Host prefix-array  storage.
 * @param host_result_bytes Host download storage.
 * @param anchors_out_growth_bytes New caller-owned anchor storage.
 * @return Component sizes and total. A total reaching uint64_t maximum is invalid.
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
 * @brief Checks that a valid memory plan fits the available budget.
 * @param plan Planned memory requirements.
 * @param allocatable_bytes Bytes available after existing reservations.
 * @return True when the plan is valid and its total fits.
 */
[[nodiscard]] inline constexpr bool vulkan_seed_reservation_fits(
    const vulkan_seed_reservation_plan& plan,
    std::uint64_t allocatable_bytes) noexcept
{
    return plan.is_arithmetic_valid && plan.total_bytes <= allocatable_bytes;
}

/** Allows shader storage access and uploads/downloads for seed buffers. */
inline constexpr VkBufferUsageFlags k_vulkan_seed_buffer_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

/**
 * @brief Plans memory for seed lookup using precomputed anchor counts.
 *
 * Queries Vulkan for each nonempty buffer because allocation sizes can  exceed
 * payload sizes. Adds host prefix and result storage, plus a new output buffer
 * when its memory is not already covered by the context allocation hook.
 * @param vulkan_runtime Runtime used to query allocation sizes.
 * @param minimizer_count Query minimizer count.
 * @param sort_plan Validated axchor-sort plan.
 * @param has_device_minimizers Query records already in device memory.
 * @param anchors_out_capacity Existing output capacity in anchor records.
 * @param context_backing_is_admitted Context allocations already reserve their own bytes.
 * @return Memory plan, invalid when size calculations reach uint64_t maximum.
 */
template <class p_minimizer_type>
[[nodiscard]] vulkan_seed_reservation_plan plan_vulkan_seed_lookup_reservation(
    bioseq::gpu::vulkan::runtime& vulkan_runtime,
    std::uint64_t minimizer_count,
    const bioseq::gpu::vulkan::seed_anchor_sort_plan& sort_plan,
    bool has_device_minimizers,
    std::uint64_t anchors_out_capacity,
    bool context_backing_is_admitted)
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
    const std::uint64_t canonical_anchor_bytes = saturating_mul(
        sort_plan.anchor_count, sizeof(bioseq::chaining::anchor));
    const std::uint64_t anchors_out_growth =
        !context_backing_is_admitted
            && anchors_out_capacity < sort_plan.anchor_count
            ? canonical_anchor_bytes
            : 0u;
    const auto requirement = [&vulkan_runtime](std::uint64_t bytes) {
        // Ask Vulkan for the allocation  size of each nonempty buffer.
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
 * @brief Estimates temporary bytes for GPU seed counting and prefix sums.
 * @param minimizer_count Query minimizer count within the GPU interface limits.
 * @param has_device_minimizers Query records already in device, memory.
 * @return Buffer payload bytes plus arena-alignment allowance, clamped on overflow
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
