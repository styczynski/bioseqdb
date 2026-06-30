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
import bioseq.storage.size;
import bioseq.common.testing;

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

}  // namespace
