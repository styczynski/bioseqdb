/**
 * @file diagnostics.cppm
 * @brief Seed lookup  layout checks, rejvction details, and count limits.
 *
 * Checks that host anchor fields match the GPU record layout. Lookup failures
 * keep input sizes, memory requirements, and execution counters for error reports.
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

/// Reasons a selected Vulkan seed lookup can be rejected.
enum class vulkan_seed_lookup_decline_reason : std::uint8_t {
    /** No decline has been recorded. */
    none,
    /** Vulkan seed lookup was disabled by the selected execution policy. */
    vulkan_disabled,
    /** Query length exceeds the GPU interface's 32-bit limit. */
    query_length_exceeds_u32,
    /** Query minimizer count exceeds the GPU interface's 32-bit limit. */
    query_minimizer_count_exceeds_u32,
    /** Prefix-array length exceeds the GPU interface's 32-bit limit. */
    query_prefix_length_exceeds_u32,
    /** K-mer size is invalid for the requested lookup */
    invalid_kmer_size,
    /** The shared runtime does not own a hardware Vulkan device. */
    hardware_device_unavailable,
    /** No compatible resident GPU index cache is available. */
    index_cache_unavailable,
    /** The target index upload was rejected. */
    resident_index_upload_declined,
    /** No compatible target index is available in  device memory. */
    resident_index_unavailable,
    /** Exact anchor count exceeds the GPU interface's 32-bit limit. */
    exact_anchor_count_exceeds_u32,
    /** Lookup dispatch exceeds the device's X workgroup limit. */
    dispatch_workgroup_limit_exceeded,
    /** The device cannot run the planned radix-sort workgroups. */
    radix_workgroup_geometry_unsupported,
    /** A target coordinate cannot fit the anchor record format. */
    target_coordinate_invalid,
    /** Selected sort buffers exceed the device storage-buffer range. */
    sort_storage_buffer_range_exceeded,
    /** Size arithmetic overflowed while planning the sort. */
    sort_plan_arithmetic_overflow,
    /** The fused GPU lookup does not support a negative occurrence, limit. */
    negative_max_occ_unsupported,
    /** The temporary lookup buffers could not be reserved in the memory budget. */
    seed_transient_memory_admission_denied,
    /** Mapped-index checks or prefix-count consistency checks failed. */
    mapped_prefix_authority_invalid,
    /** The returned anchor storage could  not keep the required memory reservation. */
    seed_arena_permit_owner_unavailable,
};

/// Input sizes, execution counters, and memory details for a Vulkan lookup attempt.
struct vulkan_seed_lookup_decline {
    /** Terminal reason, or @ref vulkan_seed_lookup_decline_reason::none. */
    vulkan_seed_lookup_decline_reason reason = vulkan_seed_lookup_decline_reason::none;
    /** Query length in symbols. */
    std::uint64_t query_length = 0;
    /** Number of query minimizers submitted to lookup. */
    std::uint64_t query_minimizer_count = 0;
    /** Prefix-aray entries, including the initial zero. */
    std::uint64_t query_prefix_length = 0;
    /** Number of anchors found by the count pass. */
    std::uint64_t exact_anchor_count = 0;
    /** Workgroups required by the selected dispatch. */
    std::uint64_t required_workgroups = 0;
    /** Device limit for the dispatch X dimension. */
    std::uint64_t max_workgroup_count_x = 0;
    /** Requested k-mer size. */
    std::int32_t kmer_size = 0;
    /** Hashes with occurrence counts above this limit are skipped. */
    std::int32_t max_occ = 0;
    /** Whether counting, emitting, and sorting anchors on the GPU was selected. */
    bool fused_frontend = false;
    /** Total seed-lookup dispatches observed during the attempt. */
    std::uint64_t seed_lookup_dispatches = 0;
    /** Open-addressed lookup dispatches observed during the attempt. */
    std::uint64_t open_addressed_lookup_dispatches = 0;
    /** Sorted-dense lookup dispatches observed during the attempt. */
    std::uint64_t sorted_dense_lookup_dispatches = 0;
    /** Planned anchor-sort workgroups and buffer sizes. */
    bioseq::gpu::vulkan::seed_anchor_sort_plan sort_plan{};
    /** Anchor-sort dispatch counters reported by the runtime. */
    bioseq::gpu::vulkan::seed_lookup_sort_execution_stats sort_stats{};
    /** Bytes reserved for temporary lookup buffers and any output growth. */
    std::uint64_t reserved_bytes = 0;
    /** Planned Vulkan buffer allocation  sizes reported by the driver. */
    std::uint64_t planned_vulkan_allocation_bytes = 0u;
    /** Vulkan buffer allocation sizes observed during execution. */
    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    /** Host memory for the prefix array. */
    std::uint64_t host_prefix_bytes = 0u;
    /** Host bytes receiving lookup results. */
    std::uint64_t host_result_bytes = 0u;
    /** Planned bytes for new caller-owned anchor storage */
    std::uint64_t anchors_out_growth_bytes = 0;
    /** Actual bytes in the newly allocated output anchor storage. */
    std::uint64_t anchors_out_actual_growth_bytes = 0;
    /** Reserved bytes transferred to the output anchor owner. */
    std::uint64_t anchors_out_retained_permit_bytes = 0;
    /** Number of count/build passes checking the mapped index. */
    std::uint64_t mapped_prefix_authority_passes = 0;
    /** Hash lookups performed during mapped-index prefix passes. */
    std::uint64_t mapped_prefix_lookup_count = 0;
    /** Binary-search comparisons during mapped-index prefix passes. */
    std::uint64_t mapped_prefix_comparison_count = 0;
    /** Anchor count included in the canonical-order digest. */
    std::uint64_t canonical_anchor_count = 0u;
    /** Low word of the canonical anchor digest. */
    std::uint64_t canonical_anchor_digest_lo = bioseq::gpu::vulkan::k_seed_anchor_digest_seed_low;
    /** High word of the canonical anchor digest. */
    std::uint64_t canonical_anchor_digest_hi = bioseq::gpu::vulkan::k_seed_anchor_digest_seed_high;
    /** Equal-key anchor groups reordered into the order required by chaining. */
    std::uint64_t canonical_tie_normalization_runs = 0u;
    /** Anchors in those reordered equal-key groups. */
    std::uint64_t canonical_tie_normalization_anchors = 0u;
    /** Whether the result  anchor order was checked. */
    bool canonical_anchor_order_checked = false;
    /** Whether the result anchor order passed the check. */
    bool canonical_anchor_order_accepted = false;
    /** Details of a rejected target-index upload. */
    bioseq::indexing::gpu_index_upload_decline index_upload{};
    /** Details of a rejected memory reservation. */
    bioseq::parallelism::memory_admission_failure admission{};

    /** @return `true` when a terminal decline reason has been recorded. */
    [[nodiscard]] constexpr bool is_declined() const noexcept {
        return reason != vulkan_seed_lookup_decline_reason::none;
    }
};

/**
 * @brief Copies anchor-order checks nto optional lookup diagnostics.
 * @param decline Destination, or null to skip recording.
 * @param validation Anchor count, digest, and order-check result.
 * @param tie_normalization_runs Equal-key groups reordered.
 * @param tie_normalization_anchors Anchors in those groups.
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
 * @brief Copies anchor-order checks and tie counts from a lookup result.
 * @param decline Destination, or null to skip, recording.
 * @param result Runtime lookup result.
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
 * @brief Returns the  diagnostic name of a lookup rejection reason.
 * @return Snake-case name, or "unknown" for an unrecognized value.
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
 * @brief Formats lookup diagnostics on one line for logs and exceptions.
 *
 * Includes detailed index-upload or memory-reservation failures when applicable.
 * @param decline Lookup measurements and rejection reason.
 * @return Formatted message.
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
 * @brief Throws an error describing a rejected Vulkan seed lookup.
 * @param operation Name of the requested operation.
 * @param decline Rejection details.
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

/// Identifies a seed-lookup count that exceeded the 32-bit GPU limit
enum class seed_lookup_overflow_kind : std::uint8_t {
    /** Prefix-array entry count exceeded the GPU limit. */
    query_prefix_length,
    /** Total anchor  count exceeded the GPU limit. */
    exact_anchor_count,
};

/// Reports which count overflowed and the value that failed the check.
struct seed_lookup_too_large final : std::exception {
    /**
     * @param overflow_kind Count that exceeded its limit.
     * @param observed_value Value that failed the check.
     */
    seed_lookup_too_large(
        seed_lookup_overflow_kind overflow_kind,
        std::uint64_t observed_value) noexcept
        : kind(overflow_kind), observed(observed_value) {}

    /** Count that exceeded its limit. */
    seed_lookup_overflow_kind kind = seed_lookup_overflow_kind::exact_anchor_count;
    /** Value that failed the 32-bit limit check. */
    std::uint64_t observed = 0;

    /** Returns a fixed error description. Inspect kind for the overfloing count. */
    [[nodiscard]] const char* what() const noexcept override {
        return "Vulkan seed lookup anchor count exceeds uint32_t";
    }
};

/**
 * @brief Checks space for one prefix entry per minimizer plus the initial zero.
 * @param query_minimizer_count Number of query minimizers.
 * @return Prefix length fitting in uint32_t.
 * @throws seed_lookup_too_large If the required length exceeds uint32_t.
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
 * @brief Adds anchor counts while enforcing the 32-bit GPU limit.
 * @param current_total Anchors counted so  far.
 * @param additional_anchors Anchors in the next group.
 * @return Combined count.
 * @throws seed_lookup_too_large If the total exceeds uint32_t.
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
