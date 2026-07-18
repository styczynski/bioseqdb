/**
 * @file seeding_vulkan.cppm
 * @brief DOC0307 Phase B — Vulkan seed-hit lookup that consumes a GPU-resident
 *        `MinimizerHashmapFragmentIndex` view and emits sorted
 *        `bioseq::chaining::Anchor` records.
 *
 * A non-allocating count prepass over the host `table` builds the exact
 * exclusive-prefix array (exact even for duplicate query minimizers), the
 * runtime runs the lookup plus bitonic-or-radix GPU sort, and the downloaded 24-byte anchors are
 * memcpy'd into the existing `ArenaSlab<Anchor>`. The index is never uploaded
 * here — `collect_seed_hits_vulkan` accepts a resident handle built through
 * `GpuMinimizerIndexCache` (DOC0307 §7, §8).
 */
module;

#include "common.h"

#include <cstring>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.sequences.seeding.vulkan;

import bioseq.common.mlogging;
import bioseq.sequences.kmers.vulkan;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_vulkan;
import bioseq.sequences.chaining.utils;
import bioseq.gpu.vulkan.phase_b_abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

export namespace bioseq::gpu::vulkan {

enum class VulkanSeedLookupDeclineReason : std::uint8_t {
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

struct VulkanSeedLookupDecline {
    VulkanSeedLookupDeclineReason reason = VulkanSeedLookupDeclineReason::none;
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
    SeedAnchorSortPlan sort_plan{};
    SeedLookupSortExecutionProof sort_proof{};
    std::uint64_t reserved_bytes = 0;
    std::uint64_t anchors_out_growth_bytes = 0;
    std::uint64_t anchors_out_actual_growth_bytes = 0;
    std::uint64_t anchors_out_retained_permit_bytes = 0;
    std::uint64_t mapped_prefix_authority_passes = 0;
    std::uint64_t mapped_prefix_lookup_count = 0;
    std::uint64_t mapped_prefix_comparison_count = 0;
    bioseq::indexing::GpuIndexUploadDecline index_upload{};
    bioseq::parallelism::MemoryAdmissionFailure admission{};

    [[nodiscard]] constexpr bool declined() const noexcept {
        return reason != VulkanSeedLookupDeclineReason::none;
    }
};

[[nodiscard]] inline constexpr std::string_view
vulkan_seed_lookup_decline_reason_name(
    VulkanSeedLookupDeclineReason reason) noexcept
{
    switch (reason) {
        case VulkanSeedLookupDeclineReason::none:
            return "none";
        case VulkanSeedLookupDeclineReason::vulkan_disabled:
            return "vulkan_disabled";
        case VulkanSeedLookupDeclineReason::query_length_exceeds_u32:
            return "query_length_exceeds_u32";
        case VulkanSeedLookupDeclineReason::query_minimizer_count_exceeds_u32:
            return "query_minimizer_count_exceeds_u32";
        case VulkanSeedLookupDeclineReason::query_prefix_length_exceeds_u32:
            return "query_prefix_length_exceeds_u32";
        case VulkanSeedLookupDeclineReason::invalid_kmer_size:
            return "invalid_kmer_size";
        case VulkanSeedLookupDeclineReason::hardware_device_unavailable:
            return "hardware_device_unavailable";
        case VulkanSeedLookupDeclineReason::index_cache_unavailable:
            return "index_cache_unavailable";
        case VulkanSeedLookupDeclineReason::resident_index_upload_declined:
            return "resident_index_upload_declined";
        case VulkanSeedLookupDeclineReason::resident_index_unavailable:
            return "resident_index_unavailable";
        case VulkanSeedLookupDeclineReason::exact_anchor_count_exceeds_u32:
            return "exact_anchor_count_exceeds_u32";
        case VulkanSeedLookupDeclineReason::dispatch_workgroup_limit_exceeded:
            return "dispatch_workgroup_limit_exceeded";
        case VulkanSeedLookupDeclineReason::radix_workgroup_geometry_unsupported:
            return "radix_workgroup_geometry_unsupported";
        case VulkanSeedLookupDeclineReason::target_coordinate_invalid:
            return "target_coordinate_invalid";
        case VulkanSeedLookupDeclineReason::sort_storage_buffer_range_exceeded:
            return "sort_storage_buffer_range_exceeded";
        case VulkanSeedLookupDeclineReason::sort_plan_arithmetic_overflow:
            return "sort_plan_arithmetic_overflow";
        case VulkanSeedLookupDeclineReason::negative_max_occ_unsupported:
            return "negative_max_occ_unsupported";
        case VulkanSeedLookupDeclineReason::seed_transient_memory_admission_denied:
            return "seed_transient_memory_admission_denied";
        case VulkanSeedLookupDeclineReason::mapped_prefix_authority_invalid:
            return "mapped_prefix_authority_invalid";
        case VulkanSeedLookupDeclineReason::seed_arena_permit_owner_unavailable:
            return "seed_arena_permit_owner_unavailable";
    }
    return "unknown";
}

[[nodiscard]] inline std::string format_vulkan_seed_lookup_decline(
    const VulkanSeedLookupDecline& decline)
{
    const std::string_view reason =
        vulkan_seed_lookup_decline_reason_name(decline.reason);
    const std::string metrics = fmt::format(
        "query_len={} query_minimizers={} prefix_len={} exact_anchors={} "
        "required_workgroups={} max_workgroup_count_x={} k={} max_occ={} fused={} "
        "sort_backend={} sort_passes={} sort_workgroups={} sort_device_bytes={} "
        "sort_host_bytes={} reserved_bytes={} anchors_out_growth_bytes={} "
        "anchors_out_actual_growth_bytes={} retained_anchor_permit_bytes={} "
        "mapped_prefix_authority_passes={} mapped_prefix_lookups={} "
        "mapped_prefix_comparisons={} "
        "seed_lookup_dispatches={} open_lookup_dispatches={} "
        "sorted_dense_lookup_dispatches={} bitonic_dispatches={} "
        "radix_operations={} radix_anchors={} "
        "radix_passes={} radix_histogram_dispatches={} radix_scan_dispatches={} "
        "radix_scatter_dispatches={} host_sort_fallbacks={}",
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
        decline.anchors_out_growth_bytes,
        decline.anchors_out_actual_growth_bytes,
        decline.anchors_out_retained_permit_bytes,
        decline.mapped_prefix_authority_passes,
        decline.mapped_prefix_lookup_count,
        decline.mapped_prefix_comparison_count,
        decline.seed_lookup_dispatches,
        decline.open_addressed_lookup_dispatches,
        decline.sorted_dense_lookup_dispatches,
        decline.sort_proof.bitonic_sort_dispatches,
        decline.sort_proof.radix_sort_operations,
        decline.sort_proof.radix_anchor_count,
        decline.sort_proof.radix_passes,
        decline.sort_proof.radix_histogram_dispatches,
        decline.sort_proof.radix_scan_dispatches,
        decline.sort_proof.radix_scatter_dispatches,
        decline.sort_proof.host_sort_fallbacks);
    if (decline.reason
        == VulkanSeedLookupDeclineReason::resident_index_upload_declined) {
        return fmt::format(
            "reason={} {} index_upload=({})",
            reason,
            metrics,
            bioseq::indexing::format_gpu_index_upload_decline(
                decline.index_upload));
    }
    if (decline.reason
        == VulkanSeedLookupDeclineReason::seed_transient_memory_admission_denied) {
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

[[noreturn]] inline void throw_vulkan_seed_lookup_strict_decline(
    std::string_view stage,
    const VulkanSeedLookupDecline& decline)
{
    throw VulkanRuntimeError(fmt::format(
        "{} selected but strict Vulkan execution was rejected ({})",
        stage,
        format_vulkan_seed_lookup_decline(decline)));
}

enum class SeedLookupOverflowKind : std::uint8_t {
    query_prefix_length,
    exact_anchor_count,
};

struct SeedLookupTooLarge final : std::exception {
    SeedLookupTooLarge(
        SeedLookupOverflowKind overflow_kind,
        std::uint64_t observed_value) noexcept
        : kind(overflow_kind), observed(observed_value) {}

    SeedLookupOverflowKind kind = SeedLookupOverflowKind::exact_anchor_count;
    std::uint64_t observed = 0;

    [[nodiscard]] const char* what() const noexcept override {
        return "Vulkan seed lookup anchor count exceeds uint32_t";
    }
};

// ===========================================================================
// Non-allocating prefix prepass (DOC0307 §7.1)
// ===========================================================================

struct SeedPrefixPlan {
    std::uint64_t prefix_len = 0;
    std::uint64_t exact_anchor_count = 0;
    std::uint64_t mapped_lookup_count = 0;
    std::uint64_t mapped_comparison_count = 0;
};

struct SeedPrefixBuildResult {
    std::vector<std::uint32_t> prefix;
    std::uint64_t mapped_lookup_count = 0u;
    std::uint64_t mapped_comparison_count = 0u;
};

struct MappedPrefixAuthorityInvalid final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "mapped sorted-dense prefix authority is invalid";
    }
};

struct MappedSortedDensePrefixView {
    std::shared_ptr<const bioseq::indexing::GpuResidentMinimizerIndex>
        resident;
    const bioseq::indexing::GpuMinimizerSlot* controls = nullptr;
    std::uint32_t bucket_count = 0u;
    std::uint32_t position_count = 0u;
    std::uint64_t lookup_count = 0u;
    std::uint64_t comparison_count = 0u;
};

[[nodiscard]] inline MappedSortedDensePrefixView
validated_mapped_sorted_dense_prefix_view(
    const bioseq::gpu::vulkan::Runtime& runtime,
    const std::shared_ptr<const bioseq::indexing::
        GpuResidentMinimizerIndex>& resident)
{
    using bioseq::indexing::GpuIndexRepresentation;
    using bioseq::indexing::GpuResidentAccountingAuthority;
    using bioseq::parallelism::saturating_mul;
    if (!resident || !resident->slots.valid()
        || !resident->positions.valid()
        || resident->slots.mapped == nullptr
        || resident->positions.mapped == nullptr
        || resident->slots.owner != &runtime
        || resident->positions.owner != &runtime
        || !resident->slots.host_visible_coherent()
        || !resident->positions.host_visible_coherent()
        || resident->representation != GpuIndexRepresentation::sorted_dense
        || !resident->key.source_direct_sorted_dense
        || resident->key.representation
            != GpuIndexRepresentation::sorted_dense
        || resident->key.sorted_dense_abi_version
            != bioseq::indexing::kMinimizerSortedDenseGpuAbiVersion
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
        || resident->key.device_uuid != runtime.features().device_uuid
        || resident->source_stream_checksum == 0u
        || resident->source_stream_checksum
            != resident->mapped_stream_checksum
        || resident->complete_source_traversals != 1u
        || resident->complete_mapped_traversals != 1u) {
        throw MappedPrefixAuthorityInvalid{};
    }
    const std::uint64_t control_bytes = saturating_mul(
        resident->bucket_count,
        sizeof(bioseq::indexing::GpuMinimizerSlot));
    const std::uint64_t position_bytes = saturating_mul(
        resident->position_count, sizeof(std::uint64_t));
    const std::uint64_t actual_allocation_sum =
        bioseq::parallelism::saturating_add(
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
        throw MappedPrefixAuthorityInvalid{};
    }
    return MappedSortedDensePrefixView{
        .resident = resident,
        .controls = static_cast<const bioseq::indexing::GpuMinimizerSlot*>(
            resident->slots.mapped),
        .bucket_count = resident->bucket_count,
        .position_count = resident->position_count,
    };
}

[[nodiscard]] inline std::uint32_t mapped_sorted_dense_position_count(
    MappedSortedDensePrefixView& view,
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
        throw MappedPrefixAuthorityInvalid{};
    }
    return view.controls[lo].pos_count;
}

template <class MinimizerT>
[[nodiscard]] SeedPrefixPlan count_seed_prefix_plan(
    std::span<const MinimizerT> query_minimizers,
    const bioseq::gpu::vulkan::Runtime& runtime,
    const std::shared_ptr<const bioseq::indexing::
        GpuResidentMinimizerIndex>& resident,
    std::int32_t max_occ)
{
    const std::uint64_t prefix_len64 = bioseq::gpu::vulkan::sat_add(
        static_cast<std::uint64_t>(query_minimizers.size()), 1u);
    if (prefix_len64 > (std::numeric_limits<std::uint32_t>::max)()) {
        throw SeedLookupTooLarge{
            SeedLookupOverflowKind::query_prefix_length,
            prefix_len64};
    }
    auto view = validated_mapped_sorted_dense_prefix_view(
        runtime, resident);
    if (max_occ < 0) {
        return SeedPrefixPlan{
            .prefix_len = prefix_len64,
            .exact_anchor_count = 0u};
    }
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0u;
    for (const MinimizerT& minimizer : query_minimizers) {
        const std::uint32_t count = mapped_sorted_dense_position_count(
            view, minimizer.kmer());
        const std::uint32_t add = count <= max_occ_u ? count : 0u;
        total = bioseq::gpu::vulkan::sat_add(total, add);
        if (total > (std::numeric_limits<std::uint32_t>::max)()) {
            throw SeedLookupTooLarge{
                SeedLookupOverflowKind::exact_anchor_count, total};
        }
    }
    return SeedPrefixPlan{
        .prefix_len = prefix_len64,
        .exact_anchor_count = total,
        .mapped_lookup_count = view.lookup_count,
        .mapped_comparison_count = view.comparison_count,
    };
}

template <class MinimizerT>
[[nodiscard]] SeedPrefixBuildResult
build_seed_prefix_counts_after_admission(
    std::span<const MinimizerT> query_minimizers,
    const bioseq::gpu::vulkan::Runtime& runtime,
    const std::shared_ptr<const bioseq::indexing::
        GpuResidentMinimizerIndex>& resident,
    std::int32_t max_occ,
    SeedPrefixPlan plan)
{
    SeedPrefixBuildResult result;
    result.prefix.resize(
        static_cast<std::size_t>(plan.prefix_len), 0u);
    auto view = validated_mapped_sorted_dense_prefix_view(
        runtime, resident);
    if (max_occ < 0) return result;
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0u;
    for (std::size_t i = 0u; i < query_minimizers.size(); ++i) {
        const std::uint32_t count = mapped_sorted_dense_position_count(
            view, query_minimizers[i].kmer());
        const std::uint32_t add = count <= max_occ_u ? count : 0u;
        total = bioseq::gpu::vulkan::sat_add(total, add);
        if (total > (std::numeric_limits<std::uint32_t>::max)()) {
            throw SeedLookupTooLarge{
                SeedLookupOverflowKind::exact_anchor_count, total};
        }
        result.prefix[i + 1u] = static_cast<std::uint32_t>(total);
    }
    if (total != plan.exact_anchor_count) {
        throw MappedPrefixAuthorityInvalid{};
    }
    result.mapped_lookup_count = view.lookup_count;
    result.mapped_comparison_count = view.comparison_count;
    return result;
}

template <class MinimizerT>
[[nodiscard]] SeedPrefixPlan count_seed_prefix_plan(
    std::span<const MinimizerT> query_minimizers,
    const bioseq::indexing::MinimizerHashmapFragmentIndex& target_index,
    std::int32_t max_occ)
{
    const std::uint64_t prefix_len64 = bioseq::gpu::vulkan::sat_add(
        static_cast<std::uint64_t>(query_minimizers.size()), 1u);
    if (prefix_len64 > std::numeric_limits<std::uint32_t>::max()) {
        throw SeedLookupTooLarge{
            SeedLookupOverflowKind::query_prefix_length,
            prefix_len64};
    }
    if (max_occ < 0) {
        return SeedPrefixPlan{.prefix_len = prefix_len64, .exact_anchor_count = 0u};
    }
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0;
    for (const MinimizerT& minimizer : query_minimizers) {
        std::uint32_t add = 0;
        const auto hits = target_index.lookup(minimizer.kmer());
        if (!hits.empty()) {
            const auto count = static_cast<std::uint32_t>(hits.size());
            if (count <= max_occ_u) add = count;
        }
        total = bioseq::gpu::vulkan::sat_add(total, static_cast<std::uint64_t>(add));
        if (total > std::numeric_limits<std::uint32_t>::max()) {
            throw SeedLookupTooLarge{
                SeedLookupOverflowKind::exact_anchor_count,
                total};
        }
    }
    return SeedPrefixPlan{.prefix_len = prefix_len64, .exact_anchor_count = total};
}

template <class MinimizerT>
[[nodiscard]] std::vector<std::uint32_t> build_seed_prefix_counts_after_admission(
    std::span<const MinimizerT> query_minimizers,
    const bioseq::indexing::MinimizerHashmapFragmentIndex& target_index,
    std::int32_t max_occ,
    SeedPrefixPlan plan)
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
        total = bioseq::gpu::vulkan::sat_add(total, static_cast<std::uint64_t>(add));
        if (total > std::numeric_limits<std::uint32_t>::max()) {
            throw SeedLookupTooLarge{
                SeedLookupOverflowKind::exact_anchor_count,
                total};
        }
        prefix[i + 1u] = static_cast<std::uint32_t>(total);
    }
    DEBUG_ASSERT(total == plan.exact_anchor_count,
        "seed prefix prepass and build pass diverged");
    return prefix;
}

// ===========================================================================
// Memory budget (DOC0307 §8, §11)
// ===========================================================================

struct VulkanSeedReservationPlan {
    std::uint64_t total_bytes = 0u;
    std::uint64_t anchors_out_growth_bytes = 0u;
};

template <class MinimizerT>
[[nodiscard]] VulkanSeedReservationPlan vulkan_seed_reserved_bytes(
    std::uint64_t minimizer_count,
    const SeedAnchorSortPlan& sort_plan,
    bool has_device_minimizers,
    std::uint64_t anchors_out_capacity) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t min_bytes = has_device_minimizers
        ? 0u
        : minimizer_wire_traits<MinimizerT>::device_bytes_for_count(minimizer_count);
    const std::uint64_t prefix_count = bioseq::gpu::vulkan::sat_add(minimizer_count, 1u);
    const std::uint64_t prefix_bytes = saturating_mul(prefix_count, sizeof(std::uint32_t));
    const std::uint64_t anchors_out_growth =
        anchors_out_capacity < sort_plan.anchor_count
            ? sort_plan.anchor_bytes : 0u;

    std::uint64_t total = 0;
    total = bioseq::gpu::vulkan::sat_add(total, min_bytes);          // optional device minimizer upload
    total = bioseq::gpu::vulkan::sat_add(total, prefix_bytes);       // host prefix vector
    total = bioseq::gpu::vulkan::sat_add(total, prefix_bytes);       // device prefix buffer
    total = bioseq::gpu::vulkan::sat_add(
        total, sort_plan.device_sort_bytes);                         // exact GPU sort buffers
    total = bioseq::gpu::vulkan::sat_add(
        total, sort_plan.host_result_bytes);                         // Runtime result vector
    total = bioseq::gpu::vulkan::sat_add(
        total, anchors_out_growth);                                  // exact ArenaSlab growth, if needed
    total = bioseq::gpu::vulkan::sat_add(total, 4u);                 // fixed direct-map helper scratch
    return VulkanSeedReservationPlan{
        .total_bytes = total,
        .anchors_out_growth_bytes = anchors_out_growth,
    };
}

template <class MinimizerT>
[[nodiscard]] std::uint64_t vulkan_phase_c_seed_count_reserved_bytes(
    std::uint64_t minimizer_count,
    bool has_device_minimizers) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t min_bytes = has_device_minimizers
        ? 0u
        : minimizer_wire_traits<MinimizerT>::device_bytes_for_count(minimizer_count);
    constexpr std::uint64_t scan_block = 256u;
    constexpr std::uint64_t arena_alignment_slack = 6u * 256u;
    const std::uint64_t num_blocks = minimizer_count == 0u
        ? 1u
        : (minimizer_count + scan_block - 1u) / scan_block;
    std::uint64_t total = 0;
    total = bioseq::gpu::vulkan::sat_add(
        total, saturating_mul(minimizer_count, sizeof(std::uint32_t)));  // counts
    total = bioseq::gpu::vulkan::sat_add(
        total, saturating_mul(minimizer_count, sizeof(std::uint32_t)));  // prefix
    total = bioseq::gpu::vulkan::sat_add(
        total, saturating_mul(num_blocks, sizeof(std::uint32_t)));       // scan blocks
    total = bioseq::gpu::vulkan::sat_add(total, 16u);                    // qrec
    total = bioseq::gpu::vulkan::sat_add(total, min_bytes);              // optional min words
    total = bioseq::gpu::vulkan::sat_add(total, arena_alignment_slack);
    return total;
}

// ===========================================================================
// Seed-hit lookup entry point (DOC0307 §8)
// ===========================================================================

struct VulkanSeedLookupCompletion {
    bool success = false;
    bioseq::parallelism::MemoryPermit retained_anchor_permit{};
    std::uint64_t retained_anchor_bytes = 0u;

    VulkanSeedLookupCompletion() = default;
    VulkanSeedLookupCompletion(bool completed) noexcept
        : success(completed) {}
    VulkanSeedLookupCompletion(VulkanSeedLookupCompletion&&) noexcept = default;
    VulkanSeedLookupCompletion& operator=(
        VulkanSeedLookupCompletion&&) noexcept = default;
    VulkanSeedLookupCompletion(const VulkanSeedLookupCompletion&) = delete;
    VulkanSeedLookupCompletion& operator=(
        const VulkanSeedLookupCompletion&) = delete;

    [[nodiscard]] operator bool() const noexcept { return success; }
};

template <class MinimizerT>
[[nodiscard]] VulkanSeedLookupCompletion collect_seed_hits_vulkan(
    std::span<const MinimizerT> query_minimizers,
    std::optional<VulkanDeviceMinimizerSpan<MinimizerT>> device_minimizers,
    const bioseq::indexing::MinimizerHashmapFragmentIndex& target_index,
    std::shared_ptr<const bioseq::indexing::GpuResidentMinimizerIndex> resident_index,
    bool vulkan_enabled,
    bioseq::storage::sequence_size_t query_len,
    std::int32_t max_occ,
    std::int32_t kmer_size,
    bioseq::chaining::ArenaSlab<bioseq::chaining::Anchor>& anchors_out,
    bool fused_frontend = false,
    VulkanSeedLookupDecline* decline_out = nullptr)
{
    if (decline_out != nullptr) {
        *decline_out = VulkanSeedLookupDecline{
            .query_length = static_cast<std::uint64_t>(query_len),
            .query_minimizer_count =
                static_cast<std::uint64_t>(query_minimizers.size()),
            .kmer_size = kmer_size,
            .max_occ = max_occ,
            .fused_frontend = fused_frontend,
        };
    }
    const auto decline = [decline_out](
            VulkanSeedLookupDeclineReason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if (query_minimizers.empty()) return true;
    if (!vulkan_enabled) {
        decline(VulkanSeedLookupDeclineReason::vulkan_disabled);
        return false;
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        decline(VulkanSeedLookupDeclineReason::query_length_exceeds_u32);
        return false;
    }
    if (kmer_size <= 0) {
        decline(VulkanSeedLookupDeclineReason::invalid_kmer_size);
        return false;
    }
    if (!resident_index) {
        decline(VulkanSeedLookupDeclineReason::resident_index_unavailable);
        return false;
    }
    if (query_minimizers.size() > std::numeric_limits<std::uint32_t>::max()) {
        decline(VulkanSeedLookupDeclineReason::
            query_minimizer_count_exceeds_u32);
        return false;
    }
    auto& runtime = Runtime::instance();
    if (!hardware_device_available(runtime)) {
        decline(VulkanSeedLookupDeclineReason::hardware_device_unavailable);
        return false;
    }
    if (decline_out != nullptr) {
        decline_out->max_workgroup_count_x =
            runtime.features().max_workgroup_count_x;
    }

    const Buffer* fused_device_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;

    // DOC0310 §14 — fused frontend: count + emit + device anchor sort on the GPU
    // (no CPU prefix prepass, no CPU anchor sort). If selected, failures are
    // hard errors; Phase B is used only when fused_frontend is false.
    if (fused_frontend && max_occ >= 0) {
        const std::uint64_t reserve =
            vulkan_phase_c_seed_count_reserved_bytes<MinimizerT>(
                query_minimizers.size(), fused_device_words != nullptr);
        auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
        bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
        auto permit = admission.try_reserve(reserve, &admission_failure);
        if (!permit) {
            if (decline_out != nullptr) {
                decline_out->reason = VulkanSeedLookupDeclineReason::
                    seed_transient_memory_admission_denied;
                decline_out->admission = admission_failure;
                return false;
            }
            throw VulkanRuntimeError(fmt::format(
                "Vulkan fused seed lookup count memory admission failed ({})",
                format_vulkan_seed_lookup_decline(VulkanSeedLookupDecline{
                    .reason = VulkanSeedLookupDeclineReason::
                        seed_transient_memory_admission_denied,
                    .query_length = static_cast<std::uint64_t>(query_len),
                    .query_minimizer_count = static_cast<std::uint64_t>(
                        query_minimizers.size()),
                    .max_workgroup_count_x =
                        runtime.features().max_workgroup_count_x,
                    .kmer_size = kmer_size,
                    .max_occ = max_occ,
                    .fused_frontend = true,
                    .admission = admission_failure,
                })));
        }
        bioseq::gpu::vulkan::PhaseCSeedInputs inputs{
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
                static_cast<std::uint32_t>(minimizer_wire_traits<MinimizerT>::word_kind),
            .minimizer_words32_per_record =
                minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            .hash_shift = minimizer_wire_traits<MinimizerT>::hash_shift,
            .pos_bits = minimizer_wire_traits<MinimizerT>::pos_bits,
            .bucket_count = resident_index->bucket_count,
            .target_position_offset = resident_index->target_position_offset,
        };
        auto anchors = runtime.run_phase_c_seed_roundtrip(
            inputs, query_len, static_cast<std::uint32_t>(kmer_size), max_occ);
        anchors_out.reserve(anchors.count);
        anchors_out.resize(anchors.count);
        if (anchors.count > 0) {
            std::memcpy(anchors_out.data(), anchors.host_bytes.data(),
                        anchors.count * sizeof(bioseq::chaining::Anchor));
        }
        if (decline_out != nullptr) {
            decline_out->seed_lookup_dispatches =
                anchors.seed_lookup_dispatches;
            decline_out->open_addressed_lookup_dispatches =
                anchors.open_addressed_lookup_dispatches;
            decline_out->sorted_dense_lookup_dispatches =
                anchors.sorted_dense_lookup_dispatches;
            decline_out->sort_proof = anchors.sort_proof;
        }
        return true;
    }

    if (fused_frontend && max_occ < 0) {
        decline(VulkanSeedLookupDeclineReason::negative_max_occ_unsupported);
        if (decline_out != nullptr) {
            return false;
        }
        throw VulkanRuntimeError(
            "Vulkan fused seed lookup selected but negative max_occ is unsupported");
    }

    const bool mapped_prefix_authority =
        resident_index->key.source_direct_sorted_dense;
    SeedPrefixPlan prefix_plan;
    try {
        prefix_plan = mapped_prefix_authority
            ? count_seed_prefix_plan(
                query_minimizers, runtime, resident_index, max_occ)
            : count_seed_prefix_plan(
                query_minimizers, target_index, max_occ);
    } catch (const SeedLookupTooLarge& overflow) {
        if (decline_out != nullptr) {
            if (overflow.kind == SeedLookupOverflowKind::query_prefix_length) {
                decline_out->reason = VulkanSeedLookupDeclineReason::
                    query_prefix_length_exceeds_u32;
                decline_out->query_prefix_length = overflow.observed;
            } else {
                decline_out->reason = VulkanSeedLookupDeclineReason::
                    exact_anchor_count_exceeds_u32;
                decline_out->exact_anchor_count = overflow.observed;
            }
        }
        return false;
    } catch (const MappedPrefixAuthorityInvalid&) {
        decline(VulkanSeedLookupDeclineReason::
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
                    query_minimizers, runtime, resident_index, max_occ);
                if (second_zero.exact_anchor_count != 0u
                    || second_zero.prefix_len != prefix_plan.prefix_len) {
                    throw MappedPrefixAuthorityInvalid{};
                }
                if (decline_out != nullptr) {
                    decline_out->mapped_prefix_authority_passes = 2u;
                    decline_out->mapped_prefix_lookup_count +=
                        second_zero.mapped_lookup_count;
                    decline_out->mapped_prefix_comparison_count +=
                        second_zero.mapped_comparison_count;
                }
            } catch (const MappedPrefixAuthorityInvalid&) {
                decline(VulkanSeedLookupDeclineReason::
                    mapped_prefix_authority_invalid);
                return false;
            }
        }
        anchors_out.resize(0);
        return true;
    }
    const SeedAnchorSortPlan sort_plan = make_seed_anchor_sort_plan(
        exact_anchors,
        resident_index->target_len,
        resident_index->target_position_offset,
        runtime.gpu_anchor_sort_capacity(),
        runtime.features().max_workgroup_count_x,
        runtime.features().max_workgroup_invocations,
        runtime.features().max_workgroup_size_x,
        runtime.features().max_storage_buffer_range);
    if (decline_out != nullptr) {
        decline_out->sort_plan = sort_plan;
    }
    if (!sort_plan.valid()) {
        switch (sort_plan.failure) {
            case SeedAnchorSortPlanFailure::anchor_count_exceeds_u32:
                decline(VulkanSeedLookupDeclineReason::
                    exact_anchor_count_exceeds_u32);
                break;
            case SeedAnchorSortPlanFailure::target_coordinate_invalid:
                decline(VulkanSeedLookupDeclineReason::
                    target_coordinate_invalid);
                break;
            case SeedAnchorSortPlanFailure::dispatch_limit_unavailable:
                decline(VulkanSeedLookupDeclineReason::
                    dispatch_workgroup_limit_exceeded);
                break;
            case SeedAnchorSortPlanFailure::radix_workgroup_geometry_unsupported:
                decline(VulkanSeedLookupDeclineReason::
                    radix_workgroup_geometry_unsupported);
                break;
            case SeedAnchorSortPlanFailure::storage_buffer_range_exceeded:
                decline(VulkanSeedLookupDeclineReason::
                    sort_storage_buffer_range_exceeded);
                break;
            case SeedAnchorSortPlanFailure::arithmetic_overflow:
                decline(VulkanSeedLookupDeclineReason::
                    sort_plan_arithmetic_overflow);
                break;
            case SeedAnchorSortPlanFailure::none:
                break;
        }
        return false;
    }
    constexpr std::uint32_t seed_lookup_local_size = 128u;
    const std::uint64_t required_workgroups =
        ceil_div_u64(query_minimizers.size(), seed_lookup_local_size);
    if (decline_out != nullptr) {
        decline_out->required_workgroups = required_workgroups;
    }
    if (required_workgroups > runtime.features().max_workgroup_count_x) {
        decline(VulkanSeedLookupDeclineReason::
            dispatch_workgroup_limit_exceeded);
        return false;
    }

    const Buffer* device_minimizer_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;
    const bool has_device_minimizers = device_minimizer_words != nullptr;

    const auto reservation = vulkan_seed_reserved_bytes<MinimizerT>(
        query_minimizers.size(), sort_plan, has_device_minimizers,
        anchors_out.capacity());
    const std::uint64_t reserve = reservation.total_bytes;
    const std::uint64_t anchors_out_growth_bytes =
        reservation.anchors_out_growth_bytes;
    if (decline_out != nullptr) {
        decline_out->reserved_bytes = reserve;
        decline_out->anchors_out_growth_bytes = anchors_out_growth_bytes;
    }

    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(reserve, &admission_failure);
    if (!permit) {
        if (decline_out != nullptr) {
            decline_out->reason = VulkanSeedLookupDeclineReason::
                seed_transient_memory_admission_denied;
            decline_out->admission = admission_failure;
        }
        return false;
    }

    if (mapped_prefix_authority
        && (anchors_out.capacity() != 0u
            || retained_anchor_permit_out == nullptr
            || static_cast<bool>(*retained_anchor_permit_out))) {
        decline(retained_anchor_permit_out == nullptr
            ? VulkanSeedLookupDeclineReason::
                seed_arena_permit_owner_unavailable
            : VulkanSeedLookupDeclineReason::
                mapped_prefix_authority_invalid);
        return false;
    }

    std::uint64_t actual_slab_growth_bytes = 0u;
    try {
        {
            std::vector<std::uint32_t> prefix;
            if (mapped_prefix_authority) {
                auto built_prefix =
                    build_seed_prefix_counts_after_admission(
                        query_minimizers,
                        runtime,
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
            RuntimeSeedLookupInputs inputs{
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
                    minimizer_wire_traits<MinimizerT>::word_kind),
                .minimizer_words32_per_record =
                    minimizer_wire_traits<MinimizerT>::
                        words32_per_minimizer,
                .hash_shift =
                    minimizer_wire_traits<MinimizerT>::hash_shift,
                .pos_bits = minimizer_wire_traits<MinimizerT>::pos_bits,
                .target_len = resident_index->target_len,
                .target_position_offset =
                    resident_index->target_position_offset,
            };
            auto anchors = runtime.run_seed_lookup_roundtrip(
                inputs,
                std::span<const std::uint32_t>{
                    prefix.data(), prefix.size()},
                query_len,
                static_cast<std::uint32_t>(kmer_size),
                exact_anchors,
                sort_plan);

            const std::uint64_t slab_capacity_before =
                anchors_out.capacity();
            anchors_out.reserve(anchors.count);
            anchors_out.resize(anchors.count);
            std::memcpy(
                anchors_out.data(),
                anchors.host_bytes.data(),
                anchors.count * sizeof(bioseq::chaining::Anchor));
            actual_slab_growth_bytes =
                bioseq::parallelism::saturating_mul(
                    anchors_out.capacity() - slab_capacity_before,
                    sizeof(bioseq::chaining::Anchor));
            if (mapped_prefix_authority
                && (anchors.count != exact_anchors
                    || actual_slab_growth_bytes
                        != anchors_out_growth_bytes)) {
                throw MappedPrefixAuthorityInvalid{};
            }
            if (decline_out != nullptr) {
                decline_out->seed_lookup_dispatches =
                    anchors.seed_lookup_dispatches;
                decline_out->open_addressed_lookup_dispatches =
                    anchors.open_addressed_lookup_dispatches;
                decline_out->sorted_dense_lookup_dispatches =
                    anchors.sorted_dense_lookup_dispatches;
                decline_out->sort_proof = anchors.sort_proof;
            }
        }

        // Prefix/result host vectors and all runtime-owned seed temporaries
        // are now destroyed.  Only the Arena::Chain slab remains live, so the
        // aggregate permit can be reconciled to that exact retained capacity
        // and transferred to the pipeline lifetime owner.
        if (mapped_prefix_authority) {
            permit->shrink_to(actual_slab_growth_bytes);
            if (permit->bytes() != actual_slab_growth_bytes) {
                throw MappedPrefixAuthorityInvalid{};
            }
            *retained_anchor_permit_out = std::move(*permit);
            if (decline_out != nullptr) {
                decline_out->anchors_out_actual_growth_bytes =
                    actual_slab_growth_bytes;
                decline_out->anchors_out_retained_permit_bytes =
                    retained_anchor_permit_out->bytes();
            }
        }
        return true;
    } catch (const MappedPrefixAuthorityInvalid&) {
        decline(VulkanSeedLookupDeclineReason::
            mapped_prefix_authority_invalid);
        return false;
    } catch (const VulkanRuntimeError&) {
        throw;
    }
}

} // namespace bioseq::gpu::vulkan
