/**
 * @file seeding_vulkan_test.cpp
 * @brief DOC0307 §13 — Vulkan seed-hit lookup parity tests.
 *
 * Test 6: `collect_seed_hits_vulkan` emits anchors with the same `ref_bits()`
 * ordering and the same anchor multiset as the CPU `collect_seed_hits` for
 * forward/reverse hits and target positions above 2^32. Skips cleanly when no
 * hardware Vulkan device exists.
 */
#include "common_simd.h"   // provides the HWY_NAMESPACE macro for collect_seed_hits
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.context;
import bioseq.sequences.kmers;
import bioseq.sequences.minimap_port;        // collect_seed_hits
import bioseq.sequences.chaining.utils;      // Anchor / ArenaSlab / make_arena_slab
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_vulkan;
import bioseq.sequences.seeding.vulkan;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

namespace {

using MinimapSketchData = bioseq::indexing::MinimapSketchData;
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using Anchor = bioseq::chaining::Anchor;
namespace ch = bioseq::chaining;
namespace vk = bioseq::gpu::vulkan;
namespace idx_ns = bioseq::indexing;

[[nodiscard]] static std::uint64_t enc(std::uint64_t pos, std::uint64_t strand) {
    return (pos << 1) | (strand & 1u);
}

/// A target index with forward/reverse repeats and one position above 2^32.
[[nodiscard]] static MinimizerHashmapFragmentIndex build_target() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.emplace_back(0x111ULL, enc(5u, 0u));
    pairs.emplace_back(0x222ULL, enc(9u, 1u));
    pairs.emplace_back(0x222ULL, enc(40u, 0u));
    pairs.emplace_back(0x222ULL, enc(100u, 1u));
    pairs.emplace_back(0x333ULL, enc(13u, 0u));
    pairs.emplace_back(0x444ULL, enc(std::uint64_t{0x1'0000'0005ULL}, 1u));  // > 2^32
    MinimizerHashmapFragmentIndex idx;
    idx.build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>>{
                             pairs.data(), pairs.size()},
                         std::uint64_t{0x2'0000'0000ULL});
    return idx;
}

[[nodiscard]] static std::vector<MinimapSketchData> build_query() {
    return {
        MinimapSketchData{0x222ULL, 7u, 0u},
        MinimapSketchData{0x444ULL, 3u, 1u},
        MinimapSketchData{0x111ULL, 20u, 0u},
        MinimapSketchData{0x999ULL, 30u, 0u},  // no target hit
    };
}

[[nodiscard]] static std::vector<std::tuple<std::uint64_t, std::uint64_t,
                                            std::uint32_t, std::uint8_t>>
anchor_tuples(const ch::ArenaSlab<Anchor>& slab) {
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint8_t>> out;
    for (std::size_t i = 0; i < slab.size(); ++i) {
        const Anchor& a = slab.data()[i];
        out.emplace_back(a.ref_position, a.query_position, a.is_reverse, a.query_span);
    }
    return out;
}

[[nodiscard]] static bool nondecreasing_ref_bits(const ch::ArenaSlab<Anchor>& slab) {
    for (std::size_t i = 1; i < slab.size(); ++i) {
        if (slab.data()[i].ref_bits() < slab.data()[i - 1].ref_bits()) return false;
    }
    return true;
}

TEST_CASE("vulkan seeding: anchors match CPU collect_seed_hits (fwd/rev, >2^32)",
          "[vulkan][seeding][phase-b]") {
    auto& runtime = vk::Runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    const auto idx = build_target();
    const auto query = build_query();
    const bioseq::storage::sequence_size_t query_len = 200;
    constexpr std::int32_t max_occ = 500;
    constexpr std::int32_t kmer_size = 15;

    context::context ctx;
    context::installed_context_scope install{ctx};

    auto cpu_slab = ch::make_arena_slab<Anchor>();
    cpu_slab.reserve(64);
    ::bioseq::simd::HWY_NAMESPACE::collect_seed_hits<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        idx, query_len, max_occ, kmer_size, cpu_slab);

    idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{64} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::GpuIndexCacheIdentity{});
    auto resident = cache.get_or_upload(runtime, idx, key, /*cache_enabled=*/true);
    ASSERT(resident != nullptr, "resident index upload failed", 0);

    auto gpu_slab = ch::make_arena_slab<Anchor>();
    gpu_slab.reserve(64);
    const bool ok = vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt, idx, resident, /*vulkan_enabled=*/true,
        query_len, max_occ, kmer_size, gpu_slab);
    ASSERT(ok, "collect_seed_hits_vulkan returned false on a usable device", 0);

    ASSERT(cpu_slab.size() == gpu_slab.size(),
           fmt::format("anchor count mismatch (cpu {} vs gpu {})",
                       cpu_slab.size(), gpu_slab.size()),
           cpu_slab.size(), gpu_slab.size());
    ASSERT(cpu_slab.size() == 5,
           fmt::format("expected 5 anchors, got {}", cpu_slab.size()),
           cpu_slab.size());
    ASSERT(nondecreasing_ref_bits(gpu_slab), "GPU anchors are not ref_bits-sorted", 0);

    const auto cpu_t = anchor_tuples(cpu_slab);
    const auto gpu_t = anchor_tuples(gpu_slab);
    ASSERT(cpu_t == gpu_t,
           "GPU anchors differ from CPU (ref/query/strand/span)", cpu_t.size());

    cache.clear();
}

TEST_CASE("vulkan seeding: empty result when no query minimizer hits the target",
          "[vulkan][seeding][phase-b]") {
    auto& runtime = vk::Runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    const auto idx = build_target();
    std::vector<MinimapSketchData> query{MinimapSketchData{0xABCULL, 1u, 0u}};

    context::context ctx;
    context::installed_context_scope install{ctx};

    idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{16} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::GpuIndexCacheIdentity{});
    auto resident = cache.get_or_upload(runtime, idx, key, true);

    auto gpu_slab = ch::make_arena_slab<Anchor>();
    gpu_slab.reserve(8);
    const bool ok = vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt, idx, resident, true, 200, 500, 15, gpu_slab);
    ASSERT(ok, "empty-hit seed lookup should still succeed", 0);
    ASSERT(gpu_slab.size() == 0, "no-hit query produced anchors", gpu_slab.size());
    cache.clear();
}

TEST_CASE("vulkan seeding: typed decline maps pre-dispatch shape predicates",
          "[vulkan][seeding][phase-b][decline]") {
    const auto query = build_query();
    const auto idx = build_target();
    auto slab = ch::make_arena_slab<Anchor>();
    vk::VulkanSeedLookupDecline decline{};

    REQUIRE_FALSE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt,
        idx,
        nullptr,
        /*vulkan_enabled=*/false,
        200,
        500,
        15,
        slab,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == vk::VulkanSeedLookupDeclineReason::vulkan_disabled);
    REQUIRE(decline.query_minimizer_count == query.size());

    REQUIRE_FALSE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt,
        idx,
        nullptr,
        /*vulkan_enabled=*/true,
        200,
        500,
        0,
        slab,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == vk::VulkanSeedLookupDeclineReason::invalid_kmer_size);

    REQUIRE_FALSE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt,
        idx,
        nullptr,
        /*vulkan_enabled=*/true,
        std::uint64_t{1} << 32,
        500,
        15,
        slab,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == vk::VulkanSeedLookupDeclineReason::query_length_exceeds_u32);

    REQUIRE_FALSE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt,
        idx,
        nullptr,
        /*vulkan_enabled=*/true,
        200,
        500,
        15,
        slab,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == vk::VulkanSeedLookupDeclineReason::resident_index_unavailable);
}

TEST_CASE("vulkan seeding: strict typed decline preserves cache and seed admission metrics",
          "[vulkan][seeding][phase-b][decline]") {
    const bioseq::parallelism::MemoryAdmissionFailure admission{
        .reason = bioseq::parallelism::MemoryAdmissionFailureReason::
            budget_exhausted,
        .requested_bytes = 84060352u,
        .reserved_bytes = 1356268448u,
        .budget_ceiling_bytes = 2193457152u,
        .allocatable_after_reservations_bytes = 837188704u,
        .process_footprint_bytes = 6284226112u,
        .system_available_bytes = 4340940800u,
        .system_total_bytes = 17179869184u,
        .process_soft_cap_bytes = 13743895347u,
    };
    const vk::VulkanSeedLookupDecline seed_decline{
        .reason = vk::VulkanSeedLookupDeclineReason::
            seed_transient_memory_admission_denied,
        .query_length = 5121827u,
        .query_minimizer_count = 1023214u,
        .query_prefix_length = 1023215u,
        .exact_anchor_count = 1023214u,
        .required_workgroups = 7994u,
        .max_workgroup_count_x = 65535u,
        .kmer_size = 15,
        .max_occ = 500,
        .fused_frontend = false,
        .admission = admission,
    };
    const std::string formatted =
        vk::format_vulkan_seed_lookup_decline(seed_decline);
    REQUIRE(formatted.find("reason=seed_transient_memory_admission_denied")
            != std::string::npos);
    REQUIRE(formatted.find("admission_reason=budget_exhausted")
            != std::string::npos);
    REQUIRE(formatted.find("required_bytes=84060352") != std::string::npos);
    REQUIRE(formatted.find("query_len=5121827") != std::string::npos);
    REQUIRE(formatted.find("exact_anchors=1023214") != std::string::npos);
    REQUIRE(formatted.find("required_workgroups=7994") != std::string::npos);

    bool caught = false;
    try {
        vk::throw_vulkan_seed_lookup_strict_decline(
            "Vulkan seed lookup fixture", seed_decline);
    } catch (const vk::VulkanRuntimeError& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("Vulkan seed lookup fixture selected")
                != std::string::npos);
        REQUIRE(message.find("seed_transient_memory_admission_denied")
                != std::string::npos);
        REQUIRE(message.find("system_total_bytes=17179869184")
                != std::string::npos);
    }
    REQUIRE(caught);

    const vk::VulkanSeedLookupDecline cache_decline{
        .reason = vk::VulkanSeedLookupDeclineReason::
            resident_index_upload_declined,
        .query_length = 5121827u,
        .query_minimizer_count = 1023214u,
        .kmer_size = 15,
        .max_occ = 500,
        .index_upload = {
            .reason = idx_ns::GpuIndexUploadDeclineReason::
                upload_peak_memory_admission_denied,
            .resident_bytes = 100u,
            .upload_peak_bytes = 200u,
            .slot_bytes = 80u,
            .position_bytes = 20u,
            .admission = admission,
        },
    };
    const std::string cache_formatted =
        vk::format_vulkan_seed_lookup_decline(cache_decline);
    REQUIRE(cache_formatted.find("reason=resident_index_upload_declined")
            != std::string::npos);
    REQUIRE(cache_formatted.find("upload_peak_memory_admission_denied")
            != std::string::npos);
    REQUIRE(cache_formatted.find("required_bytes=84060352")
            != std::string::npos);
    REQUIRE(cache_formatted.find("upload_peak_bytes=200")
            != std::string::npos);
}

TEST_CASE("vulkan seeding: every typed decline reason has a stable metric name",
          "[vulkan][seeding][phase-b][decline]") {
    for (const auto reason : {
            vk::VulkanSeedLookupDeclineReason::vulkan_disabled,
            vk::VulkanSeedLookupDeclineReason::query_length_exceeds_u32,
            vk::VulkanSeedLookupDeclineReason::query_minimizer_count_exceeds_u32,
            vk::VulkanSeedLookupDeclineReason::query_prefix_length_exceeds_u32,
            vk::VulkanSeedLookupDeclineReason::invalid_kmer_size,
            vk::VulkanSeedLookupDeclineReason::hardware_device_unavailable,
            vk::VulkanSeedLookupDeclineReason::index_cache_unavailable,
            vk::VulkanSeedLookupDeclineReason::resident_index_upload_declined,
            vk::VulkanSeedLookupDeclineReason::resident_index_unavailable,
            vk::VulkanSeedLookupDeclineReason::exact_anchor_count_exceeds_u32,
            vk::VulkanSeedLookupDeclineReason::dispatch_workgroup_limit_exceeded,
            vk::VulkanSeedLookupDeclineReason::negative_max_occ_unsupported,
            vk::VulkanSeedLookupDeclineReason::
                seed_transient_memory_admission_denied}) {
        REQUIRE(vk::vulkan_seed_lookup_decline_reason_name(reason) != "unknown");
    }
}

}  // namespace
