/**
 * @file vulkan.cppm
 * @brief Vulkan seed-hit lookup consuming a GPU-resident
 *        `minimizer_hashmap_fragment_index` view and emits sorted
 *        `bioseq::chaining::anchor` records.
 *
 * A non-allocating count prepass over the host `table` builds the exact
 * exclusive-prefix array (exact even for duplicate query minimizers), the
 * Vulkan runtime runs the lookup plus bitonic-or-radix GPU sort, and the downloaded 24-byte anchors are
 * memcpy'd into the existing `arena_slab<anchor>`. The index is never uploaded
 * here: `collect_seed_hits_vulkan` accepts an already-resident index handle.
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

export module bioseq.seeding.vulkan;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.chaining.core;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

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
    none,
    vulkan_disabled,
    query_length_exceeds_u32,
    query_minimizer_count_exceeds_u32,
    query_prefix_length_exceeds_u32,
    invalid_kmer_size,
    hardware_device_unavailable,
    index_cache_unavailable,
    resident_index_upload_declined,
    resident_index_unavailable,
    exact_anchor_count_exceeds_u32,
    dispatch_workgroup_limit_exceeded,
    radix_workgroup_geometry_unsupported,
    target_coordinate_invalid,
    sort_storage_buffer_range_exceeded,
    sort_plan_arithmetic_overflow,
    negative_max_occ_unsupported,
    seed_transient_memory_admission_denied,
    mapped_prefix_authority_invalid,
    seed_arena_permit_owner_unavailable,
};

/// Structured diagnostics for a Vulkan seed-lookup admission or execution decline.
struct vulkan_seed_lookup_decline {
    vulkan_seed_lookup_decline_reason reason = vulkan_seed_lookup_decline_reason::none;
    std::uint64_t query_length = 0;
    std::uint64_t query_minimizer_count = 0;
    std::uint64_t query_prefix_length = 0;
    std::uint64_t exact_anchor_count = 0;
    std::uint64_t required_workgroups = 0;
    std::uint64_t max_workgroup_count_x = 0;
    std::int32_t kmer_size = 0;
    std::int32_t max_occ = 0;
    bool fused_frontend = false;
    std::uint64_t seed_lookup_dispatches = 0;
    std::uint64_t open_addressed_lookup_dispatches = 0;
    std::uint64_t sorted_dense_lookup_dispatches = 0;
    bioseq::gpu::vulkan::seed_anchor_sort_plan sort_plan{};
    bioseq::gpu::vulkan::seed_lookup_sort_execution_stats sort_stats{};
    std::uint64_t reserved_bytes = 0;
    std::uint64_t planned_vulkan_allocation_bytes = 0u;
    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    std::uint64_t host_prefix_bytes = 0u;
    std::uint64_t host_result_bytes = 0u;
    std::uint64_t anchors_out_growth_bytes = 0;
    std::uint64_t anchors_out_actual_growth_bytes = 0;
    std::uint64_t anchors_out_retained_permit_bytes = 0;
    std::uint64_t mapped_prefix_authority_passes = 0;
    std::uint64_t mapped_prefix_lookup_count = 0;
    std::uint64_t mapped_prefix_comparison_count = 0;
    std::uint64_t canonical_anchor_count = 0u;
    std::uint64_t canonical_anchor_digest_lo = bioseq::gpu::vulkan::k_seed_anchor_digest_seed_low;
    std::uint64_t canonical_anchor_digest_hi = bioseq::gpu::vulkan::k_seed_anchor_digest_seed_high;
    std::uint64_t canonical_tie_normalization_runs = 0u;
    std::uint64_t canonical_tie_normalization_anchors = 0u;
    bool canonical_anchor_order_checked = false;
    bool canonical_anchor_order_accepted = false;
    bioseq::indexing::gpu_index_upload_decline index_upload{};
    bioseq::parallelism::memory_admission_failure admission{};

    /// Return whether a decline reason has been recorded.
    [[nodiscard]] constexpr bool is_declined() const noexcept {
        return reason != vulkan_seed_lookup_decline_reason::none;
    }
};

/// Copy canonical-order audit counters into @p decline when provided.
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

/// Copy canonical-order audit counters from a complete lookup result.
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

/// Return the stable diagnostic spelling of @p reason.
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

/// Format the complete structured decline for logs and strict exceptions.
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

/// Raise a strict runtime error containing @p operation and @p decline.
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
    query_prefix_length,
    exact_anchor_count,
};

/// Internal checked-arithmetic exception carrying the overflowing quantity.
struct seed_lookup_too_large final : std::exception {
    seed_lookup_too_large(
        seed_lookup_overflow_kind overflow_kind,
        std::uint64_t observed_value) noexcept
        : kind(overflow_kind), observed(observed_value) {}

    seed_lookup_overflow_kind kind = seed_lookup_overflow_kind::exact_anchor_count;
    std::uint64_t observed = 0;

    [[nodiscard]] const char* what() const noexcept override {
        return "Vulkan seed lookup anchor count exceeds uint32_t";
    }
};

/// Shared checked arithmetic for both the mapped-resident and CPU-oracle
/// prefix passes.  Keeping the boundary in a scalar seam makes the u32 ABI
/// failure testable without constructing an invalid multi-billion-element
/// span, while the production loops still exercise this exact helper.
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

// ===========================================================================
// Non-allocating prefix prepass
// ===========================================================================

/// Allocation-free count-pass result for the exclusive seed prefix.
struct seed_prefix_plan {
    std::uint64_t prefix_len = 0;
    std::uint64_t exact_anchor_count = 0;
    std::uint64_t mapped_lookup_count = 0;
    std::uint64_t mapped_comparison_count = 0;
};

/// Materialized prefix plus mapped-index validation work counters.
struct seed_prefix_build_result {
    std::vector<std::uint32_t> prefix;
    std::uint64_t mapped_lookup_count = 0u;
    std::uint64_t mapped_comparison_count = 0u;
};

/// Raised when mapped sorted-dense authority changes or fails validation.
struct mapped_prefix_invalid_error final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "mapped sorted-dense prefix authority is invalid";
    }
};

/// Validated, non-owning mapped view used by both prefix passes.
struct mapped_sorted_dense_prefix_view {
    std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index>
        resident;
    const bioseq::indexing::gpu_minimizer_slot* controls = nullptr;
    std::uint32_t bucket_count = 0u;
    std::uint32_t position_count = 0u;
    std::uint64_t lookup_count = 0u;
    std::uint64_t comparison_count = 0u;
};

/// Full mapped-payload validation result and checksum.
struct mapped_sorted_dense_payload_validation {
    std::uint64_t checksum =
        bioseq::indexing::k_gpu_source_direct_checksum_seed;
    std::uint64_t covered_position_count = 0u;
    bool canonical_order_checked = false;
    bool is_valid = false;
};

/// Allocation-free authority check used on every mapped-prefix pass.  Stored
/// metadata is not sufficient: this re-reads the complete mapped control and
/// position payload so mutation between count/build passes fails closed.
[[nodiscard]] inline mapped_sorted_dense_payload_validation
validate_mapped_sorted_dense_payload(
    std::span<const bioseq::indexing::gpu_minimizer_slot> controls,
    std::span<const std::uint64_t> positions,
    std::uint64_t target_len,
    std::uint64_t expected_checksum) noexcept
{
    mapped_sorted_dense_payload_validation validation{
        .canonical_order_checked = true,
    };
    if (controls.empty() || positions.empty() || target_len == 0u
        || expected_checksum == 0u) {
        return validation;
    }
    std::uint64_t previous_hash = 0u;
    bool have_previous_hash = false;
    for (const auto& control : controls) {
        const std::uint64_t offset = control.pos_offset;
        const std::uint64_t count = control.pos_count;
        if (count == 0u || offset != validation.covered_position_count
            || offset > positions.size()
            || count > positions.size() - offset
            || (have_previous_hash && control.hash <= previous_hash)) {
            return validation;
        }
        validation.checksum = bioseq::indexing::gpu_source_direct_checksum_word(
            validation.checksum, control.hash);
        validation.checksum = bioseq::indexing::gpu_source_direct_checksum_word(
            validation.checksum, offset);
        for (std::uint64_t i = offset; i < offset + count; ++i) {
            const std::uint64_t encoded =
                positions[static_cast<std::size_t>(i)];
            if ((encoded >> 1u) >= target_len) return validation;
            validation.checksum =
                bioseq::indexing::gpu_source_direct_checksum_word(
                    validation.checksum, encoded);
        }
        validation.checksum = bioseq::indexing::gpu_source_direct_checksum_word(
            validation.checksum, count);
        validation.covered_position_count += count;
        previous_hash = control.hash;
        have_previous_hash = true;
    }
    validation.is_valid =
        validation.covered_position_count == positions.size()
        && validation.checksum == expected_checksum;
    return validation;
}

/// Host-only validation seam shared by the runtime-resident path and bounded
/// prefix tests.  It owns no memory: callers must keep both spans alive through
/// the complete count/build pass.
[[nodiscard]] inline mapped_sorted_dense_prefix_view
validated_mapped_sorted_dense_prefix_view(
    std::span<const bioseq::indexing::gpu_minimizer_slot> controls,
    std::span<const std::uint64_t> positions,
    std::uint64_t target_len,
    std::uint64_t expected_checksum)
{
    if (controls.size()
            > (std::numeric_limits<std::uint32_t>::max)()
        || positions.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
        throw mapped_prefix_invalid_error{};
    }
    const auto payload_validation = validate_mapped_sorted_dense_payload(
        controls, positions, target_len, expected_checksum);
    if (!payload_validation.canonical_order_checked
        || !payload_validation.is_valid) {
        throw mapped_prefix_invalid_error{};
    }
    return mapped_sorted_dense_prefix_view{
        .controls = controls.data(),
        .bucket_count = static_cast<std::uint32_t>(controls.size()),
        .position_count = static_cast<std::uint32_t>(positions.size()),
    };
}

[[nodiscard]] inline mapped_sorted_dense_prefix_view
validated_mapped_sorted_dense_prefix_view(
    const bioseq::gpu::vulkan::runtime& vulkan_runtime,
    const std::shared_ptr<const bioseq::indexing::
        gpu_resident_minimizer_index>& resident)
{
    using bioseq::indexing::gpu_index_representation;
    using bioseq::indexing::gpu_resident_accounting_authority;
    using bioseq::common::saturating_mul;
    if (!resident || !resident->slots.is_valid()
        || !resident->positions.is_valid()
        || resident->slots.mapped == nullptr
        || resident->positions.mapped == nullptr
        || resident->slots.owner != &vulkan_runtime
        || resident->positions.owner != &vulkan_runtime
        || !resident->slots.is_host_visible_coherent()
        || !resident->positions.is_host_visible_coherent()
        || resident->representation
            != gpu_index_representation::source_direct_sorted_dense
        || !resident->key.source_direct_sorted_dense
        || resident->key.representation
            != gpu_index_representation::source_direct_sorted_dense
        || resident->key.sorted_dense_abi_revision
            != bioseq::indexing::k_minimizer_sorted_dense_gpu_abi_revision
        || resident->bucket_count == 0u
        || resident->bucket_count != resident->unique_key_count
        || resident->bucket_count != resident->slot_capacity
        || resident->bucket_count != resident->key.table_size
        || resident->position_count == 0u
        || resident->position_count != resident->key.position_count
        || resident->slot_mask != 0u
        || resident->target_position_offset != 0u
        || resident->gate_identity.enabled
        || resident->key.gate_enabled
        || resident->key.gate_word_count != 0u
        || resident->key.gate_mask != 0u
        || resident->key.gate_digest_lo != 0u
        || resident->key.gate_digest_hi != 0u
        || resident->key.gate_canonical_words
        || resident->key.device_uuid != vulkan_runtime.features().device_uuid
        || resident->source_stream_checksum == 0u
        || resident->source_stream_checksum
            != resident->mapped_stream_checksum
        || resident->complete_source_traversals != 1u
        || resident->complete_mapped_traversals != 1u) {
        throw mapped_prefix_invalid_error{};
    }
    const std::uint64_t control_bytes = saturating_mul(
        resident->bucket_count,
        sizeof(bioseq::indexing::gpu_minimizer_slot));
    const std::uint64_t position_bytes = saturating_mul(
        resident->position_count, sizeof(std::uint64_t));
    const std::uint64_t actual_allocation_sum =
        bioseq::common::saturating_add(
            resident->slots.allocation_bytes,
            resident->positions.allocation_bytes);
    if (control_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || position_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || control_bytes > resident->slots.bytes
        || position_bytes > resident->positions.bytes
        || control_bytes > resident->slots.allocation_bytes
        || position_bytes > resident->positions.allocation_bytes
        || !bioseq::indexing::gpu_exact_resident_authority_valid(
            resident->accounting_authority,
            resident->materialization,
            static_cast<bool>(resident->permit),
            resident->permit.bytes(),
            actual_allocation_sum)) {
        throw mapped_prefix_invalid_error{};
    }
    const auto controls = std::span<const bioseq::indexing::gpu_minimizer_slot>{
        static_cast<const bioseq::indexing::gpu_minimizer_slot*>(
            resident->slots.mapped),
        resident->bucket_count};
    const auto positions = std::span<const std::uint64_t>{
        static_cast<const std::uint64_t*>(resident->positions.mapped),
        resident->position_count};
    auto view = validated_mapped_sorted_dense_prefix_view(
        controls,
        positions,
        static_cast<std::uint64_t>(resident->target_len),
        resident->source_stream_checksum);
    view.resident = resident;
    return view;
}

[[nodiscard]] inline std::uint32_t mapped_sorted_dense_position_count(
    mapped_sorted_dense_prefix_view& view,
    std::uint64_t hash)
{
    ++view.lookup_count;
    std::uint32_t lo = 0u;
    std::uint32_t hi = view.bucket_count;
    while (lo < hi) {
        ++view.comparison_count;
        const std::uint32_t mid = lo + ((hi - lo) >> 1u);
        if (view.controls[mid].hash < hash) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == view.bucket_count || view.controls[lo].hash != hash) {
        return 0u;
    }
    const std::uint64_t offset = view.controls[lo].pos_offset;
    const std::uint64_t count = view.controls[lo].pos_count;
    if (count == 0u || offset > view.position_count
        || count > view.position_count - offset) {
        throw mapped_prefix_invalid_error{};
    }
    return view.controls[lo].pos_count;
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_plan count_seed_prefix_plan(
    std::span<const p_minimizer_type> query_minimizers,
    mapped_sorted_dense_prefix_view view,
    std::int32_t max_occ)
{
    const std::uint64_t prefix_len64 = checked_seed_prefix_length(
        static_cast<std::uint64_t>(query_minimizers.size()));
    if (max_occ < 0) {
        return seed_prefix_plan{
            .prefix_len = prefix_len64,
            .exact_anchor_count = 0u};
    }
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0u;
    for (const p_minimizer_type& minimizer : query_minimizers) {
        const std::uint32_t count = mapped_sorted_dense_position_count(
            view, minimizer.kmer());
        const std::uint32_t add = count <= max_occ_u ? count : 0u;
        total = checked_seed_anchor_total(total, add);
    }
    return seed_prefix_plan{
        .prefix_len = prefix_len64,
        .exact_anchor_count = total,
        .mapped_lookup_count = view.lookup_count,
        .mapped_comparison_count = view.comparison_count,
    };
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_plan count_seed_prefix_plan(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::gpu::vulkan::runtime& vulkan_runtime,
    const std::shared_ptr<const bioseq::indexing::
        gpu_resident_minimizer_index>& resident,
    std::int32_t max_occ)
{
    auto view = validated_mapped_sorted_dense_prefix_view(
        vulkan_runtime, resident);
    return count_seed_prefix_plan(
        query_minimizers, std::move(view), max_occ);
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_build_result
build_seed_prefix_counts_after_admission(
    std::span<const p_minimizer_type> query_minimizers,
    mapped_sorted_dense_prefix_view view,
    std::int32_t max_occ,
    seed_prefix_plan plan)
{
    seed_prefix_build_result result;
    result.prefix.resize(
        static_cast<std::size_t>(plan.prefix_len), 0u);
    if (max_occ < 0) return result;
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0u;
    for (std::size_t i = 0u; i < query_minimizers.size(); ++i) {
        const std::uint32_t count = mapped_sorted_dense_position_count(
            view, query_minimizers[i].kmer());
        const std::uint32_t add = count <= max_occ_u ? count : 0u;
        total = checked_seed_anchor_total(total, add);
        result.prefix[i + 1u] = static_cast<std::uint32_t>(total);
    }
    if (total != plan.exact_anchor_count) {
        throw mapped_prefix_invalid_error{};
    }
    result.mapped_lookup_count = view.lookup_count;
    result.mapped_comparison_count = view.comparison_count;
    return result;
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_build_result
build_seed_prefix_counts_after_admission(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::gpu::vulkan::runtime& vulkan_runtime,
    const std::shared_ptr<const bioseq::indexing::
        gpu_resident_minimizer_index>& resident,
    std::int32_t max_occ,
    seed_prefix_plan plan)
{
    auto view = validated_mapped_sorted_dense_prefix_view(
        vulkan_runtime, resident);
    return build_seed_prefix_counts_after_admission(
        query_minimizers, std::move(view), max_occ, plan);
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_plan count_seed_prefix_plan(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::indexing::minimizer_hashmap_fragment_index& target_index,
    std::int32_t max_occ)
{
    const std::uint64_t prefix_len64 = checked_seed_prefix_length(
        static_cast<std::uint64_t>(query_minimizers.size()));
    if (max_occ < 0) {
        return seed_prefix_plan{.prefix_len = prefix_len64, .exact_anchor_count = 0u};
    }
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0;
    for (const p_minimizer_type& minimizer : query_minimizers) {
        std::uint32_t add = 0;
        const auto hits = target_index.lookup(minimizer.kmer());
        if (!hits.empty()) {
            const auto count = static_cast<std::uint32_t>(hits.size());
            if (count <= max_occ_u) add = count;
        }
        total = checked_seed_anchor_total(total, add);
    }
    return seed_prefix_plan{.prefix_len = prefix_len64, .exact_anchor_count = total};
}

template <class p_minimizer_type>
[[nodiscard]] std::vector<std::uint32_t> build_seed_prefix_counts_after_admission(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::indexing::minimizer_hashmap_fragment_index& target_index,
    std::int32_t max_occ,
    seed_prefix_plan plan)
{
    std::vector<std::uint32_t> prefix(static_cast<std::size_t>(plan.prefix_len), 0u);
    if (max_occ < 0) return prefix;

    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < query_minimizers.size(); ++i) {
        std::uint32_t add = 0;
        const auto hits = target_index.lookup(query_minimizers[i].kmer());
        if (!hits.empty()) {
            const auto count = static_cast<std::uint32_t>(hits.size());
            if (count <= max_occ_u) add = count;
        }
        total = checked_seed_anchor_total(total, add);
        prefix[i + 1u] = static_cast<std::uint32_t>(total);
    }
    DEBUG_ASSERT(total == plan.exact_anchor_count,
        "seed prefix prepass and build pass diverged");
    return prefix;
}

// ===========================================================================
// Memory reservation
// ===========================================================================

/// Exact host, device, and retained-output reservation for one lookup.
struct vulkan_seed_reservation_plan {
    std::uint64_t total_bytes = 0u;
    std::uint64_t vulkan_allocation_requirement_bytes = 0u;
    std::uint64_t host_prefix_bytes = 0u;
    std::uint64_t host_result_bytes = 0u;
    std::uint64_t anchors_out_growth_bytes = 0u;
    bool is_arithmetic_valid = false;
};

/// Driver-accounted Vulkan buffer allocation requirements by purpose.
struct vulkan_seed_buffer_allocation_requirements {
    std::uint64_t scratch_bytes = 0u;
    std::uint64_t uploaded_minimizer_bytes = 0u;
    std::uint64_t prefix_bytes = 0u;
    std::uint64_t anchors_bytes = 0u;
    std::uint64_t anchor_keys_bytes = 0u;
    std::uint64_t sort_ranges_bytes = 0u;
    std::uint64_t anchors_scratch_bytes = 0u;
    std::uint64_t anchor_keys_scratch_bytes = 0u;
    std::uint64_t histogram_bytes = 0u;

    /// Return the saturated sum of all buffer requirements.
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

/// Compose a complete lookup reservation from device and host requirements.
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

/// Return whether @p plan is arithmetically valid and fits @p allocatable_bytes.
[[nodiscard]] inline constexpr bool vulkan_seed_reservation_fits(
    const vulkan_seed_reservation_plan& plan,
    std::uint64_t allocatable_bytes) noexcept
{
    return plan.is_arithmetic_valid && plan.total_bytes <= allocatable_bytes;
}

inline constexpr VkBufferUsageFlags k_vulkan_seed_buffer_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

/// Plan a prefix-driven Vulkan seed lookup.
/// @tparam p_minimizer_type Minimizer record type.
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

/// Return the saturated transient reservation for fused GPU seed counting.
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

// ===========================================================================
// Seed-hit lookup entry point
// ===========================================================================

/// Move-only completion state and retained output-memory reservation.
struct vulkan_seed_lookup_completion {
    bool is_successful = false;
    bioseq::parallelism::memory_permit retained_anchor_permit{};
    std::uint64_t retained_anchor_bytes = 0u;

    vulkan_seed_lookup_completion() = default;
    vulkan_seed_lookup_completion(bool completed) noexcept
        : is_successful(completed) {}
    vulkan_seed_lookup_completion(vulkan_seed_lookup_completion&&) noexcept = default;
    vulkan_seed_lookup_completion& operator=(
        vulkan_seed_lookup_completion&&) noexcept = default;
    vulkan_seed_lookup_completion(const vulkan_seed_lookup_completion&) = delete;
    vulkan_seed_lookup_completion& operator=(
        const vulkan_seed_lookup_completion&) = delete;

    /// Return whether lookup completed without a recorded decline.
    [[nodiscard]] operator bool() const noexcept { return is_successful; }
};

/**
 * @brief Collect canonical anchors through the selected Vulkan seed route.
 *
 * The function never silently selects a CPU lookup. It returns a false
 * completion only for a structured, caller-visible decline and propagates
 * runtime failures once GPU execution begins.
 */
template <class p_minimizer_type>
[[nodiscard]] vulkan_seed_lookup_completion collect_seed_hits_vulkan(
    std::span<const p_minimizer_type> query_minimizers,
    std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer_type>>
        device_minimizers,
    const bioseq::indexing::minimizer_hashmap_fragment_index& target_index,
    std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index> resident_index,
    bool vulkan_enabled,
    bioseq::storage::sequence_size_t query_len,
    std::int32_t max_occ,
    std::int32_t kmer_size,
    bioseq::chaining::arena_slab<bioseq::chaining::anchor>& anchors_out,
    bool fused_frontend = false,
    vulkan_seed_lookup_decline* decline_out = nullptr)
{
    if (decline_out != nullptr) {
        *decline_out = vulkan_seed_lookup_decline{
            .query_length = static_cast<std::uint64_t>(query_len),
            .query_minimizer_count =
                static_cast<std::uint64_t>(query_minimizers.size()),
            .kmer_size = kmer_size,
            .max_occ = max_occ,
            .fused_frontend = fused_frontend,
        };
    }
    const auto decline = [decline_out](
            vulkan_seed_lookup_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    const bool source_direct_empty_minimizer_query =
        query_minimizers.empty()
        && resident_index != nullptr
        && resident_index->key.source_direct_sorted_dense;
    if (query_minimizers.empty()
        && !source_direct_empty_minimizer_query) {
        record_vulkan_seed_anchor_audit(
            decline_out, bioseq::gpu::vulkan::audit_seed_anchor_canonical_order({}, 0u));
        return true;
    }
    if (!vulkan_enabled) {
        decline(vulkan_seed_lookup_decline_reason::vulkan_disabled);
        return false;
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_seed_lookup_decline_reason::query_length_exceeds_u32);
        return false;
    }
    if (kmer_size <= 0) {
        decline(vulkan_seed_lookup_decline_reason::invalid_kmer_size);
        return false;
    }
    if (!resident_index) {
        decline(vulkan_seed_lookup_decline_reason::resident_index_unavailable);
        return false;
    }
    if (query_minimizers.size() > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_seed_lookup_decline_reason::
            query_minimizer_count_exceeds_u32);
        return false;
    }
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime)) {
        decline(vulkan_seed_lookup_decline_reason::hardware_device_unavailable);
        return false;
    }
    if (decline_out != nullptr) {
        decline_out->max_workgroup_count_x =
            vulkan_runtime.features().max_workgroup_count_x;
    }

    const bool mapped_prefix_authority =
        resident_index->key.source_direct_sorted_dense;
    if (mapped_prefix_authority && fused_frontend) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    }

    const bioseq::gpu::vulkan::buffer* fused_device_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;

    // The fused frontend counts, emits, and sorts anchors on the GPU without a
    // CPU prefix prepass or CPU anchor sort. Once selected, any failure is a
    // hard error; the prefix-driven path is used only when it is not selected.
    if (fused_frontend && max_occ >= 0) {
        const std::uint64_t reserve =
            vulkan_fused_seed_lookup_reservation_bytes<p_minimizer_type>(
                query_minimizers.size(), fused_device_words != nullptr);
        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        bioseq::parallelism::memory_admission_failure admission_failure{};
        auto permit = admission.try_reserve(reserve, &admission_failure);
        if (!permit) {
            if (decline_out != nullptr) {
                decline_out->reason = vulkan_seed_lookup_decline_reason::
                    seed_transient_memory_admission_denied;
                decline_out->admission = admission_failure;
                return false;
            }
            throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                "Vulkan fused seed lookup count memory admission failed ({})",
                format_vulkan_seed_lookup_decline(vulkan_seed_lookup_decline{
                    .reason = vulkan_seed_lookup_decline_reason::
                        seed_transient_memory_admission_denied,
                    .query_length = static_cast<std::uint64_t>(query_len),
                    .query_minimizer_count = static_cast<std::uint64_t>(
                        query_minimizers.size()),
                    .max_workgroup_count_x =
                        vulkan_runtime.features().max_workgroup_count_x,
                    .kmer_size = kmer_size,
                    .max_occ = max_occ,
                    .fused_frontend = true,
                    .admission = admission_failure,
                })));
        }
        bioseq::gpu::vulkan::gpu_seed_inputs inputs{
            .host_minimizer_bytes = fused_device_words != nullptr
                ? std::span<const std::byte>{}
                : std::as_bytes(query_minimizers),
            .device_minimizer_words = fused_device_words,
            .slots = &resident_index->slots,
            .positions = &resident_index->positions,
            .target_len = resident_index->target_len,
            .slot_mask = resident_index->slot_mask,
            .position_count = resident_index->position_count,
            .minimizer_count = static_cast<std::uint32_t>(query_minimizers.size()),
            .minimizer_word_kind =
                static_cast<std::uint32_t>(bioseq::gpu::vulkan::
                    minimizer_wire_traits<p_minimizer_type>::k_word_kind),
            .minimizer_words32_per_record =
                bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::
                    k_words32_per_minimizer,
            .hash_shift = bioseq::gpu::vulkan::
                minimizer_wire_traits<p_minimizer_type>::k_hash_shift,
            .pos_bits = bioseq::gpu::vulkan::
                minimizer_wire_traits<p_minimizer_type>::k_position_bits,
            .bucket_count = resident_index->bucket_count,
            .target_position_offset = resident_index->target_position_offset,
        };
        auto anchors = vulkan_runtime.run_seed_anchor_roundtrip(
            inputs, query_len, static_cast<std::uint32_t>(kmer_size), max_occ);
        if (!anchors.anchor_order_audit.canonical_order_checked
            || !anchors.anchor_order_audit.canonical_order_accepted
            || anchors.anchor_order_audit.anchor_count != anchors.count) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan fused seed anchor validation is invalid");
        }
        anchors_out.reserve(anchors.count);
        anchors_out.resize(anchors.count);
        if (anchors.count > 0) {
            std::memcpy(anchors_out.data(), anchors.host_bytes.data(),
                        anchors.count * sizeof(bioseq::chaining::anchor));
        }
        if (decline_out != nullptr) {
            decline_out->seed_lookup_dispatches =
                anchors.seed_lookup_dispatches;
            decline_out->open_addressed_lookup_dispatches =
                anchors.open_addressed_lookup_dispatches;
            decline_out->sorted_dense_lookup_dispatches =
                anchors.sorted_dense_lookup_dispatches;
            decline_out->sort_stats = anchors.sort_stats;
            decline_out->exact_anchor_count = anchors.count;
            record_vulkan_seed_anchor_audit(decline_out, anchors);
        }
        return true;
    }

    if (fused_frontend && max_occ < 0) {
        decline(vulkan_seed_lookup_decline_reason::negative_max_occ_unsupported);
        if (decline_out != nullptr) {
            return false;
        }
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "Vulkan fused seed lookup selected but negative max_occ is unsupported");
    }

    if (mapped_prefix_authority && anchors_out.capacity() != 0u) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    }
    seed_prefix_plan prefix_plan;
    try {
        prefix_plan = mapped_prefix_authority
            ? count_seed_prefix_plan(
                query_minimizers, vulkan_runtime, resident_index, max_occ)
            : count_seed_prefix_plan(
                query_minimizers, target_index, max_occ);
    } catch (const seed_lookup_too_large& overflow) {
        if (decline_out != nullptr) {
            if (overflow.kind == seed_lookup_overflow_kind::query_prefix_length) {
                decline_out->reason = vulkan_seed_lookup_decline_reason::
                    query_prefix_length_exceeds_u32;
                decline_out->query_prefix_length = overflow.observed;
            } else {
                decline_out->reason = vulkan_seed_lookup_decline_reason::
                    exact_anchor_count_exceeds_u32;
                decline_out->exact_anchor_count = overflow.observed;
            }
        }
        return false;
    } catch (const mapped_prefix_invalid_error&) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    }
    const std::uint64_t exact_anchors = prefix_plan.exact_anchor_count;
    if (decline_out != nullptr) {
        decline_out->query_prefix_length = prefix_plan.prefix_len;
        decline_out->exact_anchor_count = exact_anchors;
        decline_out->mapped_prefix_authority_passes =
            mapped_prefix_authority ? 1u : 0u;
        decline_out->mapped_prefix_lookup_count =
            prefix_plan.mapped_lookup_count;
        decline_out->mapped_prefix_comparison_count =
            prefix_plan.mapped_comparison_count;
    }
    if (exact_anchors == 0u) {
        if (mapped_prefix_authority) {
            try {
                const auto second_zero = count_seed_prefix_plan(
                    query_minimizers, vulkan_runtime, resident_index, max_occ);
                if (second_zero.exact_anchor_count != 0u
                    || second_zero.prefix_len != prefix_plan.prefix_len) {
                    throw mapped_prefix_invalid_error{};
                }
                if (decline_out != nullptr) {
                    decline_out->mapped_prefix_authority_passes = 2u;
                    decline_out->mapped_prefix_lookup_count +=
                        second_zero.mapped_lookup_count;
                    decline_out->mapped_prefix_comparison_count +=
                        second_zero.mapped_comparison_count;
                }
            } catch (const mapped_prefix_invalid_error&) {
                decline(vulkan_seed_lookup_decline_reason::
                    mapped_prefix_authority_invalid);
                return false;
            }
        }
        anchors_out.resize(0);
        record_vulkan_seed_anchor_audit(
            decline_out, bioseq::gpu::vulkan::audit_seed_anchor_canonical_order({}, 0u));
        return true;
    }
    const bioseq::gpu::vulkan::seed_anchor_sort_plan sort_plan = bioseq::gpu::vulkan::make_seed_anchor_sort_plan(
        exact_anchors,
        resident_index->target_len,
        resident_index->target_position_offset,
        vulkan_runtime.gpu_anchor_sort_capacity(),
        vulkan_runtime.features().max_workgroup_count_x,
        vulkan_runtime.features().max_workgroup_invocations,
        vulkan_runtime.features().max_workgroup_size_x,
        vulkan_runtime.features().max_storage_buffer_range);
    if (decline_out != nullptr) {
        decline_out->sort_plan = sort_plan;
    }
    if (!sort_plan.is_valid()) {
        switch (sort_plan.failure) {
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::anchor_count_exceeds_u32:
                decline(vulkan_seed_lookup_decline_reason::
                    exact_anchor_count_exceeds_u32);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::target_coordinate_invalid:
                decline(vulkan_seed_lookup_decline_reason::
                    target_coordinate_invalid);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::dispatch_limit_unavailable:
                decline(vulkan_seed_lookup_decline_reason::
                    dispatch_workgroup_limit_exceeded);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::radix_workgroup_geometry_unsupported:
                decline(vulkan_seed_lookup_decline_reason::
                    radix_workgroup_geometry_unsupported);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::storage_buffer_range_exceeded:
                decline(vulkan_seed_lookup_decline_reason::
                    sort_storage_buffer_range_exceeded);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::arithmetic_overflow:
                decline(vulkan_seed_lookup_decline_reason::
                    sort_plan_arithmetic_overflow);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::none:
                break;
        }
        return false;
    }
    constexpr std::uint32_t k_seed_lookup_local_size = 128u;
    const std::uint64_t required_workgroups =
        bioseq::common::ceil_div(
            query_minimizers.size(), k_seed_lookup_local_size);
    if (decline_out != nullptr) {
        decline_out->required_workgroups = required_workgroups;
    }
    if (required_workgroups > vulkan_runtime.features().max_workgroup_count_x) {
        decline(vulkan_seed_lookup_decline_reason::
            dispatch_workgroup_limit_exceeded);
        return false;
    }

    const bioseq::gpu::vulkan::buffer* device_minimizer_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;
    const bool has_device_minimizers = device_minimizer_words != nullptr;

    const auto reservation = plan_vulkan_seed_lookup_reservation<p_minimizer_type>(
        vulkan_runtime,
        query_minimizers.size(), sort_plan, has_device_minimizers,
        anchors_out.capacity());
    if (!reservation.is_arithmetic_valid) {
        decline(vulkan_seed_lookup_decline_reason::sort_plan_arithmetic_overflow);
        return false;
    }
    const std::uint64_t reserve = reservation.total_bytes;
    const std::uint64_t anchors_out_growth_bytes =
        reservation.anchors_out_growth_bytes;
    if (decline_out != nullptr) {
        decline_out->reserved_bytes = reserve;
        decline_out->planned_vulkan_allocation_bytes =
            reservation.vulkan_allocation_requirement_bytes;
        decline_out->host_prefix_bytes = reservation.host_prefix_bytes;
        decline_out->host_result_bytes = reservation.host_result_bytes;
        decline_out->anchors_out_growth_bytes = anchors_out_growth_bytes;
    }

    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance()
        .try_reserve(reserve, &admission_failure);
    if (!permit) {
        if (decline_out != nullptr) {
            decline_out->reason = vulkan_seed_lookup_decline_reason::
                seed_transient_memory_admission_denied;
            decline_out->admission = admission_failure;
        }
        return false;
    }

    std::uint64_t actual_slab_growth_bytes = 0u;
    bool post_allocation_invariant_failed = false;
    try {
        {
            std::vector<std::uint32_t> prefix;
            if (mapped_prefix_authority) {
                auto built_prefix =
                    build_seed_prefix_counts_after_admission(
                        query_minimizers,
                        vulkan_runtime,
                        resident_index,
                        max_occ,
                        prefix_plan);
                if (decline_out != nullptr) {
                    decline_out->mapped_prefix_authority_passes = 2u;
                    decline_out->mapped_prefix_lookup_count +=
                        built_prefix.mapped_lookup_count;
                    decline_out->mapped_prefix_comparison_count +=
                        built_prefix.mapped_comparison_count;
                }
                prefix = std::move(built_prefix.prefix);
            } else {
                prefix = build_seed_prefix_counts_after_admission(
                    query_minimizers, target_index, max_occ, prefix_plan);
            }
            bioseq::gpu::vulkan::runtime_seed_lookup_inputs inputs{
                .host_minimizer_bytes = has_device_minimizers
                    ? std::span<const std::byte>{}
                    : std::as_bytes(query_minimizers),
                .device_minimizer_words = device_minimizer_words,
                .slots = &resident_index->slots,
                .positions = &resident_index->positions,
                .slot_mask = resident_index->slot_mask,
                .bucket_count = resident_index->bucket_count,
                .position_count = resident_index->position_count,
                .minimizer_word_kind = static_cast<std::uint32_t>(
                    bioseq::gpu::vulkan::minimizer_wire_traits<
                        p_minimizer_type>::k_word_kind),
                .minimizer_words32_per_record =
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::
                        k_words32_per_minimizer,
                .hash_shift =
                    bioseq::gpu::vulkan::minimizer_wire_traits<
                        p_minimizer_type>::k_hash_shift,
                .pos_bits = bioseq::gpu::vulkan::minimizer_wire_traits<
                    p_minimizer_type>::k_position_bits,
                .target_len = resident_index->target_len,
                .target_position_offset =
                    resident_index->target_position_offset,
            };
            auto anchors = vulkan_runtime.run_seed_lookup_roundtrip(
                inputs,
                std::span<const std::uint32_t>{
                    prefix.data(), prefix.size()},
                query_len,
                static_cast<std::uint32_t>(kmer_size),
                exact_anchors,
                sort_plan,
                reservation.vulkan_allocation_requirement_bytes);

            if (anchors.count != exact_anchors
                || anchors.actual_vulkan_allocation_bytes
                    != reservation.vulkan_allocation_requirement_bytes
                || !anchors.anchor_order_audit.canonical_order_checked
                || !anchors.anchor_order_audit.canonical_order_accepted
                || anchors.anchor_order_audit.anchor_count != anchors.count) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan seed lookup result validation disagrees with admission");
            }

            const std::uint64_t slab_capacity_before =
                anchors_out.capacity();
            anchors_out.reserve(anchors.count);
            anchors_out.resize(anchors.count);
            std::memcpy(
                anchors_out.data(),
                anchors.host_bytes.data(),
                anchors.count * sizeof(bioseq::chaining::anchor));
            actual_slab_growth_bytes =
                bioseq::common::saturating_mul(
                    anchors_out.capacity() - slab_capacity_before,
                    sizeof(bioseq::chaining::anchor));
            if (mapped_prefix_authority
                && actual_slab_growth_bytes
                    != anchors_out_growth_bytes) {
                post_allocation_invariant_failed = true;
            }
            if (decline_out != nullptr) {
                decline_out->seed_lookup_dispatches =
                    anchors.seed_lookup_dispatches;
                decline_out->open_addressed_lookup_dispatches =
                    anchors.open_addressed_lookup_dispatches;
                decline_out->sorted_dense_lookup_dispatches =
                    anchors.sorted_dense_lookup_dispatches;
                decline_out->sort_stats = anchors.sort_stats;
                decline_out->actual_vulkan_allocation_bytes =
                    anchors.actual_vulkan_allocation_bytes;
                record_vulkan_seed_anchor_audit(decline_out, anchors);
            }
        }

        // Prefix/result host vectors and runtime-owned seed temporaries have
        // left scope. Reconcile the aggregate permit to the exact capacity
        // retained by the per-query chain arena before transferring ownership.
        if (mapped_prefix_authority) {
            permit->shrink_to(actual_slab_growth_bytes);
            if (permit->bytes() != actual_slab_growth_bytes) {
                post_allocation_invariant_failed = true;
            }
            if (decline_out != nullptr) {
                decline_out->anchors_out_actual_growth_bytes =
                    actual_slab_growth_bytes;
                decline_out->anchors_out_retained_permit_bytes =
                    permit->bytes();
            }
            vulkan_seed_lookup_completion completion{
                !post_allocation_invariant_failed};
            completion.retained_anchor_bytes = permit->bytes();
            completion.retained_anchor_permit = std::move(*permit);
            if (post_allocation_invariant_failed) {
                decline(vulkan_seed_lookup_decline_reason::
                    mapped_prefix_authority_invalid);
            }
            return completion;
        }
        return true;
    } catch (const mapped_prefix_invalid_error&) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    } catch (const bioseq::gpu::vulkan::vulkan_runtime_error&) {
        throw;
    }
}

} // namespace bioseq::seeding
