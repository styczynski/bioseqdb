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
    SeedAnchorSortPlan sort_plan{};
    SeedLookupSortExecutionProof sort_proof{};
    std::uint64_t reserved_bytes = 0;
    std::uint64_t anchors_out_growth_bytes = 0;
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
        "seed_lookup_dispatches={} bitonic_dispatches={} radix_operations={} radix_anchors={} "
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
        decline.seed_lookup_dispatches,
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
};

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

template <class MinimizerT>
[[nodiscard]] std::uint64_t vulkan_seed_reserved_bytes(
    std::uint64_t minimizer_count,
    const SeedAnchorSortPlan& sort_plan,
    bool has_device_minimizers,
    std::uint64_t anchors_out_capacity,
    std::uint64_t* anchors_out_growth_bytes = nullptr) noexcept
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
    if (anchors_out_growth_bytes != nullptr) {
        *anchors_out_growth_bytes = anchors_out_growth;
    }

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
    return total;
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

template <class MinimizerT>
[[nodiscard]] bool collect_seed_hits_vulkan(
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

    SeedPrefixPlan prefix_plan;
    try {
        prefix_plan = count_seed_prefix_plan(query_minimizers, target_index, max_occ);
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
    }
    const std::uint64_t exact_anchors = prefix_plan.exact_anchor_count;
    if (decline_out != nullptr) {
        decline_out->query_prefix_length = prefix_plan.prefix_len;
        decline_out->exact_anchor_count = exact_anchors;
    }
    if (exact_anchors == 0u) {
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

    std::uint64_t anchors_out_growth_bytes = 0u;
    const std::uint64_t reserve = vulkan_seed_reserved_bytes<MinimizerT>(
        query_minimizers.size(), sort_plan, has_device_minimizers,
        anchors_out.capacity(), &anchors_out_growth_bytes);
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

    try {
        std::vector<std::uint32_t> prefix = build_seed_prefix_counts_after_admission(
            query_minimizers, target_index, max_occ, prefix_plan);
        RuntimeSeedLookupInputs inputs{
            .host_minimizer_bytes = has_device_minimizers
                ? std::span<const std::byte>{}
                : std::as_bytes(query_minimizers),
            .device_minimizer_words = device_minimizer_words,
            .slots = &resident_index->slots,
            .positions = &resident_index->positions,
            .slot_mask = resident_index->slot_mask,
            .position_count = resident_index->position_count,
            .minimizer_word_kind =
                static_cast<std::uint32_t>(minimizer_wire_traits<MinimizerT>::word_kind),
            .minimizer_words32_per_record =
                minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            .hash_shift = minimizer_wire_traits<MinimizerT>::hash_shift,
            .pos_bits = minimizer_wire_traits<MinimizerT>::pos_bits,
            .target_len = resident_index->target_len,
            .target_position_offset =
                resident_index->target_position_offset,
        };
        auto anchors = runtime.run_seed_lookup_roundtrip(
            inputs,
            std::span<const std::uint32_t>{prefix.data(), prefix.size()},
            query_len,
            static_cast<std::uint32_t>(kmer_size),
            exact_anchors,
            sort_plan);

        anchors_out.reserve(anchors.count);
        anchors_out.resize(anchors.count);
        std::memcpy(
            anchors_out.data(),
            anchors.host_bytes.data(),
            anchors.count * sizeof(bioseq::chaining::Anchor));
        if (decline_out != nullptr) {
            decline_out->seed_lookup_dispatches =
                anchors.seed_lookup_dispatches;
            decline_out->sort_proof = anchors.sort_proof;
        }
        return true;
    } catch (const VulkanRuntimeError&) {
        throw;
    }
}

} // namespace bioseq::gpu::vulkan
