/**
 * @file seeding_vulkan.cppm
 * @brief DOC0307 Phase B — Vulkan seed-hit lookup that consumes a GPU-resident
 *        `MinimizerHashmapFragmentIndex` view and emits sorted
 *        `bioseq::chaining::Anchor` records.
 *
 * A non-allocating count prepass over the host `table` builds the exact
 * exclusive-prefix array (exact even for duplicate query minimizers), the
 * runtime runs the lookup + bitonic sort, and the downloaded 24-byte anchors are
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

struct SeedLookupTooLarge final : std::exception {
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
        throw SeedLookupTooLarge{};
    }
    if (max_occ < 0) {
        return SeedPrefixPlan{.prefix_len = prefix_len64, .exact_anchor_count = 0u};
    }
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0;
    for (const MinimizerT& minimizer : query_minimizers) {
        std::uint32_t add = 0;
        if (auto it = target_index.table.find(minimizer.kmer());
            it != target_index.table.end())
        {
            const auto count = static_cast<std::uint32_t>(it->second);
            if (count <= max_occ_u) add = count;
        }
        total = bioseq::gpu::vulkan::sat_add(total, static_cast<std::uint64_t>(add));
        if (total > std::numeric_limits<std::uint32_t>::max()) {
            throw SeedLookupTooLarge{};
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
        if (auto it = target_index.table.find(query_minimizers[i].kmer());
            it != target_index.table.end())
        {
            const auto count = static_cast<std::uint32_t>(it->second);
            if (count <= max_occ_u) add = count;
        }
        total = bioseq::gpu::vulkan::sat_add(total, static_cast<std::uint64_t>(add));
        if (total > std::numeric_limits<std::uint32_t>::max()) {
            throw SeedLookupTooLarge{};
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
    std::uint64_t exact_anchor_count,
    bool has_device_minimizers) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t min_bytes = has_device_minimizers
        ? 0u
        : minimizer_wire_traits<MinimizerT>::device_bytes_for_count(minimizer_count);
    const std::uint64_t prefix_count = bioseq::gpu::vulkan::sat_add(minimizer_count, 1u);
    const std::uint64_t prefix_bytes = saturating_mul(prefix_count, sizeof(std::uint32_t));
    const std::uint64_t anchor_bytes =
        saturating_mul(exact_anchor_count, sizeof(bioseq::chaining::Anchor));
    const std::uint64_t sort_range_count = exact_anchor_count == 0u ? 0u : 1u;
    const std::uint64_t sort_range_bytes =
        saturating_mul(sort_range_count, sizeof(SortRangeWire));
    const std::uint64_t staging_bytes =
        std::max({min_bytes, prefix_bytes, anchor_bytes, sort_range_bytes});

    std::uint64_t total = 0;
    total = bioseq::gpu::vulkan::sat_add(total, min_bytes);          // optional device minimizer upload
    total = bioseq::gpu::vulkan::sat_add(total, prefix_bytes);       // host prefix vector
    total = bioseq::gpu::vulkan::sat_add(total, prefix_bytes);       // device prefix buffer
    total = bioseq::gpu::vulkan::sat_add(total, anchor_bytes);       // device anchor buffer
    total = bioseq::gpu::vulkan::sat_add(total, anchor_bytes);       // Runtime host download bytes
    total = bioseq::gpu::vulkan::sat_add(total, sort_range_bytes);   // host sort range vector
    total = bioseq::gpu::vulkan::sat_add(total, sort_range_bytes);   // device sort range buffer
    total = bioseq::gpu::vulkan::sat_add(total, staging_bytes);      // one reusable transfer buffer
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
    bool fused_frontend = false)
{
    if (query_minimizers.empty()) return true;
    if (!vulkan_enabled) return false;
    if (query_len > std::numeric_limits<std::uint32_t>::max()) return false;
    if (kmer_size <= 0) return false;
    if (!resident_index) return false;
    if (query_minimizers.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    auto& runtime = Runtime::instance();
    if (!runtime.usable()) return false;

    const Buffer* fused_device_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;

    // DOC0310 §14 — fused frontend: count + emit + device anchor sort on the GPU
    // (no CPU prefix prepass, no CPU anchor sort). If selected, failures are
    // hard errors; Phase B is used only when fused_frontend is false.
    if (fused_frontend && max_occ >= 0) {
        const std::uint64_t reserve = vulkan_seed_reserved_bytes<MinimizerT>(
            query_minimizers.size(), query_minimizers.size(), fused_device_words != nullptr);
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(reserve);
        if (!permit) {
            throw VulkanRuntimeError(fmt::format(
                "Vulkan fused seed lookup memory admission failed for {} bytes",
                reserve));
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
        return true;
    }

    if (fused_frontend && max_occ < 0) {
        throw VulkanRuntimeError(
            "Vulkan fused seed lookup selected but negative max_occ is unsupported");
    }

    SeedPrefixPlan prefix_plan;
    try {
        prefix_plan = count_seed_prefix_plan(query_minimizers, target_index, max_occ);
    } catch (const SeedLookupTooLarge&) {
        return false;
    }
    const std::uint64_t exact_anchors = prefix_plan.exact_anchor_count;
    if (exact_anchors == 0u) {
        anchors_out.resize(0);
        return true;
    }
    constexpr std::uint32_t seed_lookup_local_size = 128u;
    if (ceil_div_u64(query_minimizers.size(), seed_lookup_local_size) >
        runtime.features().max_workgroup_count_x) {
        return false;
    }

    const Buffer* device_minimizer_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;
    const bool has_device_minimizers = device_minimizer_words != nullptr;

    const std::uint64_t reserve = vulkan_seed_reserved_bytes<MinimizerT>(
        query_minimizers.size(), exact_anchors, has_device_minimizers);

    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(reserve);
    if (!permit) return false;

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
        };
        auto anchors = runtime.run_seed_lookup_roundtrip(
            inputs,
            std::span<const std::uint32_t>{prefix.data(), prefix.size()},
            query_len,
            static_cast<std::uint32_t>(kmer_size),
            exact_anchors);

        anchors_out.reserve(anchors.count);
        anchors_out.resize(anchors.count);
        std::memcpy(
            anchors_out.data(),
            anchors.host_bytes.data(),
            anchors.count * sizeof(bioseq::chaining::Anchor));
        return true;
    } catch (const VulkanRuntimeError&) {
        throw;
    }
}

} // namespace bioseq::gpu::vulkan
