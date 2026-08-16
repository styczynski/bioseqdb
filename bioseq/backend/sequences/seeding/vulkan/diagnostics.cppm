/**
 * @file diagnostics.cppm
 * @brief Seed-lookup ABI checks, decline diagnostics, and checked bounds.
 *
 * The host anchor layout is proven against its device wire record before any
 * dispatch surface is exported. Admission and execution declines retain their
 * exact geometry, memory, routing, and canonical-order evidence for logs and
 * strict selected-backend failures.
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

export module bioseq.seeding.vulkan:diagnostics;

import bioseq.common.checked_arithmetic;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.chaining.core;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;

export namespace bioseq::seeding {

static_assert(std::is_trivially_copyable_v<bioseq::chaining::anchor>);
static_assert(std::is_standard_layout_v<bioseq::chaining::anchor>);
static_assert(sizeof(bioseq::chaining::anchor) == sizeof(bioseq::gpu::vulkan::seed_anchor_wire));
static_assert(offsetof(bioseq::chaining::anchor, ref_position)
              == offsetof(bioseq::gpu::vulkan::seed_anchor_wire, ref_position));
static_assert(offsetof(bioseq::chaining::anchor, query_position)
              == offsetof(bioseq::gpu::vulkan::seed_anchor_wire, query_position));
static_assert(offsetof(bioseq::chaining::anchor, is_reverse)
              == offsetof(bioseq::gpu::vulkan::seed_anchor_wire, is_reverse));
static_assert(offsetof(bioseq::chaining::anchor, query_span)
              == offsetof(bioseq::gpu::vulkan::seed_anchor_wire, query_span));
static_assert(offsetof(bioseq::chaining::anchor, flags)
              == offsetof(bioseq::gpu::vulkan::seed_anchor_wire, flags));
static_assert(offsetof(bioseq::chaining::anchor, segment_id)
              == offsetof(bioseq::gpu::vulkan::seed_anchor_wire, segment_id));

/// Fail-closed reasons for declining an explicitly selected Vulkan lookup.
enum class vulkan_seed_lookup_decline_reason : std::uint8_t {
    /** No decline has been recorded. */
    none,
    /** Vulkan seed lookup was disabled by the selected execution policy. */
    vulkan_disabled,
    /** Query length cannot be represented by the 32-bit device ABI. */
    query_length_exceeds_u32,
    /** Query minimizer count cannot be represented by the 32-bit device ABI. */
    query_minimizer_count_exceeds_u32,
    /** Exclusive-prefix length cannot be represented by the 32-bit device ABI. */
    query_prefix_length_exceeds_u32,
    /** Requested k-mer size is outside the selected lookup contract. */
    invalid_kmer_size,
    /** The shared runtime does not own a hardware Vulkan device. */
    hardware_device_unavailable,
    /** No compatible resident GPU index cache is available. */
    index_cache_unavailable,
    /** Uploading the selected resident index was explicitly declined. */
    resident_index_upload_declined,
    /** A compatible resident index could not be acquired after upload. */
    resident_index_unavailable,
    /** Exact anchor count cannot be represented by the 32-bit device ABI. */
    exact_anchor_count_exceeds_u32,
    /** Lookup dispatch exceeds the device's X workgroup limit. */
    dispatch_workgroup_limit_exceeded,
    /** Selected radix sort geometry is unsupported by the device. */
    radix_workgroup_geometry_unsupported,
    /** At least one target coordinate is outside the canonical wire domain. */
    target_coordinate_invalid,
    /** Selected sort buffers exceed the device storage-buffer range. */
    sort_storage_buffer_range_exceeded,
    /** Sort-plan size arithmetic saturated. */
    sort_plan_arithmetic_overflow,
    /** A negative occurrence cap cannot be represented by the device lookup. */
    negative_max_occ_unsupported,
    /** Transient seed memory admission rejected the planned reservation. */
    seed_transient_memory_admission_denied,
    /** The mapped exclusive-prefix authority failed validation. */
    mapped_prefix_authority_invalid,
    /** The caller cannot retain the required output-arena memory permit. */
    seed_arena_permit_owner_unavailable,
};

/// Structured diagnostics for a Vulkan seed-lookup admission or execution decline.
struct vulkan_seed_lookup_decline {
    /** Terminal reason, or @ref vulkan_seed_lookup_decline_reason::none. */
    vulkan_seed_lookup_decline_reason reason = vulkan_seed_lookup_decline_reason::none;
    /** Query length in logical symbols. */
    std::uint64_t query_length = 0;
    /** Number of query minimizers submitted to lookup. */
    std::uint64_t query_minimizer_count = 0;
    /** Number of entries required by the exclusive-prefix array. */
    std::uint64_t query_prefix_length = 0;
    /** Exact anchors reported by the count/prefix pass. */
    std::uint64_t exact_anchor_count = 0;
    /** Workgroups required by the selected dispatch. */
    std::uint64_t required_workgroups = 0;
    /** Device limit for the dispatch X dimension. */
    std::uint64_t max_workgroup_count_x = 0;
    /** Requested k-mer size. */
    std::int32_t kmer_size = 0;
    /** Requested maximum occurrence count. */
    std::int32_t max_occ = 0;
    /** Whether the fused minimizer-and-seed frontend was selected. */
    bool fused_frontend = false;
    /** Total seed-lookup dispatches observed during the attempt. */
    std::uint64_t seed_lookup_dispatches = 0;
    /** Open-addressed lookup dispatches observed during the attempt. */
    std::uint64_t open_addressed_lookup_dispatches = 0;
    /** Sorted-dense lookup dispatches observed during the attempt. */
    std::uint64_t sorted_dense_lookup_dispatches = 0;
    /** Selected anchor-sort geometry and storage estimate. */
    bioseq::gpu::vulkan::seed_anchor_sort_plan sort_plan{};
    /** Anchor-sort dispatch counters reported by the runtime. */
    bioseq::gpu::vulkan::seed_lookup_sort_execution_stats sort_stats{};
    /** Total transient bytes reserved through memory admission. */
    std::uint64_t reserved_bytes = 0;
    /** Planned sum of driver-accounted Vulkan allocation requirements. */
    std::uint64_t planned_vulkan_allocation_bytes = 0u;
    /** Actual driver-accounted Vulkan allocation requirements. */
    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    /** Host bytes retaining the exclusive-prefix array. */
    std::uint64_t host_prefix_bytes = 0u;
    /** Host bytes receiving lookup results. */
    std::uint64_t host_result_bytes = 0u;
    /** Planned caller-retained anchor growth. */
    std::uint64_t anchors_out_growth_bytes = 0;
    /** Actual caller-retained anchor growth. */
    std::uint64_t anchors_out_actual_growth_bytes = 0;
    /** Memory permit bytes transferred to the retained anchor owner. */
    std::uint64_t anchors_out_retained_permit_bytes = 0;
    /** Number of mapped-prefix authority validations. */
    std::uint64_t mapped_prefix_authority_passes = 0;
    /** Prefix entries inspected by mapped-prefix validation. */
    std::uint64_t mapped_prefix_lookup_count = 0;
    /** Prefix comparisons performed by mapped-prefix validation. */
    std::uint64_t mapped_prefix_comparison_count = 0;
    /** Anchor count included in the canonical-order digest. */
    std::uint64_t canonical_anchor_count = 0u;
    /** Low word of the canonical anchor digest. */
    std::uint64_t canonical_anchor_digest_lo = bioseq::gpu::vulkan::k_seed_anchor_digest_seed_low;
    /** High word of the canonical anchor digest. */
    std::uint64_t canonical_anchor_digest_hi = bioseq::gpu::vulkan::k_seed_anchor_digest_seed_high;
    /** Number of equal-key runs normalized to canonical order. */
    std::uint64_t canonical_tie_normalization_runs = 0u;
    /** Number of anchors participating in tie normalization. */
    std::uint64_t canonical_tie_normalization_anchors = 0u;
    /** Whether canonical anchor order was checked. */
    bool canonical_anchor_order_checked = false;
    /** Whether canonical anchor order passed validation. */
    bool canonical_anchor_order_accepted = false;
    /** Nested evidence for a resident-index upload decline. */
    bioseq::indexing::gpu_index_upload_decline index_upload{};
    /** Nested evidence for a transient-memory admission decline. */
    bioseq::parallelism::memory_admission_failure admission{};

    /** @return `true` when a terminal decline reason has been recorded. */
    [[nodiscard]] constexpr bool is_declined() const noexcept {
        return reason != vulkan_seed_lookup_decline_reason::none;
    }
};

/**
 * @brief Copy canonical-order audit counters into an optional decline record.
 * @param decline Destination diagnostics, or `nullptr` when reporting is disabled.
 * @param validation Canonical count, digest, and order-validation evidence.
 * @param tie_normalization_runs Number of equal-key runs normalized.
 * @param tie_normalization_anchors Number of anchors participating in normalization.
 */
inline void record_vulkan_seed_anchor_audit(
    vulkan_seed_lookup_decline* decline,
    const bioseq::gpu::vulkan::seed_anchor_canonical_order_audit& validation,
    std::uint64_t tie_normalization_runs = 0u,
    std::uint64_t tie_normalization_anchors = 0u) noexcept
{
    if (decline == nullptr) return;
    decline->canonical_anchor_count = validation.anchor_count;
    decline->canonical_anchor_digest_lo = validation.digest_lo;
    decline->canonical_anchor_digest_hi = validation.digest_hi;
    decline->canonical_anchor_order_checked =
        validation.canonical_order_checked;
    decline->canonical_anchor_order_accepted =
        validation.canonical_order_accepted;
    decline->canonical_tie_normalization_runs = tie_normalization_runs;
    decline->canonical_tie_normalization_anchors =
        tie_normalization_anchors;
}

/**
 * @brief Copy canonical-order audit counters from a complete lookup result.
 * @param decline Destination diagnostics, or `nullptr` when reporting is disabled.
 * @param result Lookup result carrying canonical validation and tie counters.
 */
inline void record_vulkan_seed_anchor_audit(
    vulkan_seed_lookup_decline* decline,
    const bioseq::gpu::vulkan::seed_lookup_result& result) noexcept
{
    record_vulkan_seed_anchor_audit(
        decline,
        result.anchor_order_audit,
        result.canonical_tie_normalization_runs,
        result.canonical_tie_normalization_anchors);
}

/**
 * @brief Return the stable diagnostic spelling of a decline reason.
 * @param reason Decline reason to name.
 * @return Stable snake-case spelling, or `unknown` for an invalid enum value.
 */
[[nodiscard]] inline constexpr std::string_view
vulkan_seed_lookup_decline_reason_name(
    vulkan_seed_lookup_decline_reason reason) noexcept
{
    switch (reason) {
        case vulkan_seed_lookup_decline_reason::none:
            return "none";
        case vulkan_seed_lookup_decline_reason::vulkan_disabled:
            return "vulkan_disabled";
        case vulkan_seed_lookup_decline_reason::query_length_exceeds_u32:
            return "query_length_exceeds_u32";
        case vulkan_seed_lookup_decline_reason::query_minimizer_count_exceeds_u32:
            return "query_minimizer_count_exceeds_u32";
        case vulkan_seed_lookup_decline_reason::query_prefix_length_exceeds_u32:
            return "query_prefix_length_exceeds_u32";
        case vulkan_seed_lookup_decline_reason::invalid_kmer_size:
            return "invalid_kmer_size";
        case vulkan_seed_lookup_decline_reason::hardware_device_unavailable:
            return "hardware_device_unavailable";
        case vulkan_seed_lookup_decline_reason::index_cache_unavailable:
            return "index_cache_unavailable";
        case vulkan_seed_lookup_decline_reason::resident_index_upload_declined:
            return "resident_index_upload_declined";
        case vulkan_seed_lookup_decline_reason::resident_index_unavailable:
            return "resident_index_unavailable";
        case vulkan_seed_lookup_decline_reason::exact_anchor_count_exceeds_u32:
            return "exact_anchor_count_exceeds_u32";
        case vulkan_seed_lookup_decline_reason::dispatch_workgroup_limit_exceeded:
            return "dispatch_workgroup_limit_exceeded";
        case vulkan_seed_lookup_decline_reason::radix_workgroup_geometry_unsupported:
            return "radix_workgroup_geometry_unsupported";
        case vulkan_seed_lookup_decline_reason::target_coordinate_invalid:
            return "target_coordinate_invalid";
        case vulkan_seed_lookup_decline_reason::sort_storage_buffer_range_exceeded:
            return "sort_storage_buffer_range_exceeded";
        case vulkan_seed_lookup_decline_reason::sort_plan_arithmetic_overflow:
            return "sort_plan_arithmetic_overflow";
        case vulkan_seed_lookup_decline_reason::negative_max_occ_unsupported:
            return "negative_max_occ_unsupported";
        case vulkan_seed_lookup_decline_reason::seed_transient_memory_admission_denied:
            return "seed_transient_memory_admission_denied";
        case vulkan_seed_lookup_decline_reason::mapped_prefix_authority_invalid:
            return "mapped_prefix_authority_invalid";
        case vulkan_seed_lookup_decline_reason::seed_arena_permit_owner_unavailable:
            return "seed_arena_permit_owner_unavailable";
    }
    return "unknown";
}

/**
 * @brief Format a complete structured decline for logs and strict exceptions.
 * @param decline Admission and execution evidence to format.
 * @return Single-line reason and metric record, including nested index-upload
 * or memory-admission evidence when applicable.
 */
[[nodiscard]] inline std::string format_vulkan_seed_lookup_decline(
    const vulkan_seed_lookup_decline& decline)
{
    const std::string_view reason =
        vulkan_seed_lookup_decline_reason_name(decline.reason);
    const std::string metrics = fmt::format(
        "query_len={} query_minimizers={} prefix_len={} exact_anchors={} "
        "required_workgroups={} max_workgroup_count_x={} k={} max_occ={} fused={} "
        "sort_backend={} sort_passes={} sort_workgroups={} sort_device_bytes={} "
        "sort_host_bytes={} reserved_bytes={} planned_vulkan_bytes={} "
        "actual_vulkan_bytes={} host_prefix_bytes={} host_result_bytes={} "
        "anchors_out_growth_bytes={} "
        "anchors_out_actual_growth_bytes={} retained_anchor_permit_bytes={} "
        "mapped_prefix_authority_passes={} mapped_prefix_lookups={} "
        "mapped_prefix_comparisons={} "
        "seed_lookup_dispatches={} open_lookup_dispatches={} "
        "sorted_dense_lookup_dispatches={} bitonic_dispatches={} "
        "radix_operations={} radix_anchors={} "
        "radix_passes={} radix_histogram_dispatches={} radix_scan_dispatches={} "
        "radix_scatter_dispatches={} host_sort_fallbacks={} "
        "canonical_anchor_count={} canonical_anchor_digest_lo={} "
        "canonical_anchor_digest_hi={} canonical_order_checked={} "
        "canonical_order_accepted={} canonical_tie_runs={} "
        "canonical_tie_anchors={}",
        decline.query_length,
        decline.query_minimizer_count,
        decline.query_prefix_length,
        decline.exact_anchor_count,
        decline.required_workgroups,
        decline.max_workgroup_count_x,
        decline.kmer_size,
        decline.max_occ,
        decline.fused_frontend ? 1 : 0,
        static_cast<std::uint32_t>(decline.sort_plan.backend),
        decline.sort_plan.radix_pass_count,
        decline.sort_plan.num_workgroups,
        decline.sort_plan.device_sort_bytes,
        decline.sort_plan.host_result_bytes,
        decline.reserved_bytes,
        decline.planned_vulkan_allocation_bytes,
        decline.actual_vulkan_allocation_bytes,
        decline.host_prefix_bytes,
        decline.host_result_bytes,
        decline.anchors_out_growth_bytes,
        decline.anchors_out_actual_growth_bytes,
        decline.anchors_out_retained_permit_bytes,
        decline.mapped_prefix_authority_passes,
        decline.mapped_prefix_lookup_count,
        decline.mapped_prefix_comparison_count,
        decline.seed_lookup_dispatches,
        decline.open_addressed_lookup_dispatches,
        decline.sorted_dense_lookup_dispatches,
        decline.sort_stats.bitonic_sort_dispatches,
        decline.sort_stats.radix_sort_operations,
        decline.sort_stats.radix_anchor_count,
        decline.sort_stats.radix_passes,
        decline.sort_stats.radix_histogram_dispatches,
        decline.sort_stats.radix_scan_dispatches,
        decline.sort_stats.radix_scatter_dispatches,
        decline.sort_stats.host_sort_fallbacks,
        decline.canonical_anchor_count,
        decline.canonical_anchor_digest_lo,
        decline.canonical_anchor_digest_hi,
        decline.canonical_anchor_order_checked ? 1 : 0,
        decline.canonical_anchor_order_accepted ? 1 : 0,
        decline.canonical_tie_normalization_runs,
        decline.canonical_tie_normalization_anchors);
    if (decline.reason
        == vulkan_seed_lookup_decline_reason::resident_index_upload_declined) {
        return fmt::format(
            "reason={} {} index_upload=({})",
            reason,
            metrics,
            bioseq::indexing::format_gpu_index_upload_decline(
                decline.index_upload));
    }
    if (decline.reason
        == vulkan_seed_lookup_decline_reason::seed_transient_memory_admission_denied) {
        const auto& admission = decline.admission;
        return fmt::format(
            "reason={} admission_reason={} required_bytes={} "
            "allocatable_after_reservations_bytes={} reserved_bytes={} "
            "budget_ceiling_bytes={} process_footprint_bytes={} "
            "system_available_bytes={} system_total_bytes={} "
            "process_soft_cap_bytes={} {}",
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
            metrics);
    }
    return fmt::format("reason={} {}", reason, metrics);
}

/**
 * @brief Raise a strict selected-backend error containing complete diagnostics.
 * @param operation Human-readable name of the selected Vulkan operation.
 * @param decline Structured evidence explaining why execution was rejected.
 * @throws bioseq::gpu::vulkan::vulkan_runtime_error Always.
 */
[[noreturn]] inline void throw_vulkan_seed_lookup_strict_decline(
    std::string_view operation,
    const vulkan_seed_lookup_decline& decline)
{
    throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
        "{} selected but strict Vulkan execution was rejected ({})",
        operation,
        format_vulkan_seed_lookup_decline(decline)));
}

/// Bounded seed-lookup quantity that exceeded the 32-bit device ABI.
enum class seed_lookup_overflow_kind : std::uint8_t {
    /** Exclusive-prefix entry count exceeded the device ABI. */
    query_prefix_length,
    /** Accumulated exact anchor count exceeded the device ABI. */
    exact_anchor_count,
};

/// Internal checked-arithmetic exception carrying the overflowing quantity.
struct seed_lookup_too_large final : std::exception {
    /**
     * @param overflow_kind Quantity whose checked accumulation failed.
     * @param observed_value First unrepresentable value.
     */
    seed_lookup_too_large(
        seed_lookup_overflow_kind overflow_kind,
        std::uint64_t observed_value) noexcept
        : kind(overflow_kind), observed(observed_value) {}

    /** Quantity whose checked accumulation failed. */
    seed_lookup_overflow_kind kind = seed_lookup_overflow_kind::exact_anchor_count;
    /** First value outside the 32-bit device domain. */
    std::uint64_t observed = 0;

    /** @return Stable exception description for the checked ABI boundary. */
    [[nodiscard]] const char* what() const noexcept override {
        return "Vulkan seed lookup anchor count exceeds uint32_t";
    }
};

/**
 * @brief Compute the checked exclusive-prefix length for a minimizer count.
 * @param query_minimizer_count Number of minimizers addressed by the prefix.
 * @return `query_minimizer_count + 1` within the 32-bit device domain.
 * @throws seed_lookup_too_large When the prefix length exceeds `std::uint32_t`.
 *
 * Both mapped-resident and CPU-oracle prefix passes share this scalar seam, so
 * overflow remains testable without constructing a multi-billion-element span.
 */
[[nodiscard]] inline std::uint64_t checked_seed_prefix_length(
    std::uint64_t query_minimizer_count)
{
    const std::uint64_t prefix_len = bioseq::common::saturating_add(
        query_minimizer_count, 1u);
    if (prefix_len > (std::numeric_limits<std::uint32_t>::max)()) {
        throw seed_lookup_too_large{
            seed_lookup_overflow_kind::query_prefix_length,
            prefix_len};
    }
    return prefix_len;
}

/**
 * @brief Accumulate an exact anchor count within the 32-bit device domain.
 * @param current_total Anchors accumulated before the current group.
 * @param additional_anchors Anchors contributed by the current group.
 * @return Checked combined anchor count.
 * @throws seed_lookup_too_large When the combined count exceeds `std::uint32_t`.
 */
[[nodiscard]] inline std::uint64_t checked_seed_anchor_total(
    std::uint64_t current_total,
    std::uint32_t additional_anchors)
{
    const std::uint64_t total = bioseq::common::saturating_add(
        current_total, static_cast<std::uint64_t>(additional_anchors));
    if (total > (std::numeric_limits<std::uint32_t>::max)()) {
        throw seed_lookup_too_large{
            seed_lookup_overflow_kind::exact_anchor_count,
            total};
    }
    return total;
}


}  // namespace bioseq::seeding
