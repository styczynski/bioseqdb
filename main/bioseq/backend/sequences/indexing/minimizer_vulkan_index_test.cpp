/**
 * @file minimizer_vulkan_index_test.cpp
 * @brief DOC0307 §13 — GPU minimizer index view + cache identity tests.
 *
 * Tests 5/9: the open-addressed GPU slot table derived from
 * `MinimizerHashmapFragmentIndex` resolves the same (hash -> positions) hits as
 * the CPU `lookup`, and distinct `assembly_identity` values (e.g. different
 * `add_position_offset`) produce distinct cache keys / hashes. The slot/key
 * tests are pure-host; the resident upload test skips when no device exists.
 */
#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <vector>

import bioseq.common.mlogging;
import bioseq.sequences.kmers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_vulkan;
import bioseq.gpu.vulkan.runtime;
import bioseq.common.testing;

namespace {

using MinimapSketchData = bioseq::indexing::MinimapSketchData;
using bioseq::indexing::MinimizerHashmapFragmentIndex;
namespace idx_ns = bioseq::indexing;

/// Host simulation of the shader's open-addressed `find_slot`.
[[nodiscard]] static std::uint32_t host_find_slot(
    const idx_ns::GpuMinimizerIndexUpload& upload, std::uint64_t hash) {
    std::uint32_t p = static_cast<std::uint32_t>(idx_ns::splitmix64(hash)) & upload.slot_mask;
    for (std::uint32_t step = 0; step <= upload.slot_mask; ++step) {
        const auto& slot = upload.slots[p];
        if (slot.pos_count == 0) return 0xffffffffu;
        if (slot.hash == hash) return p;
        p = (p + 1u) & upload.slot_mask;
    }
    return 0xffffffffu;
}

[[nodiscard]] static MinimizerHashmapFragmentIndex build_demo_index() {
    std::vector<MinimapSketchData> minimizers;
    // Several distinct kmers; one with multiple positions (a repeat).
    minimizers.emplace_back(0x111ULL, 5u, 0u);
    minimizers.emplace_back(0x222ULL, 9u, 1u);
    minimizers.emplace_back(0x333ULL, 13u, 0u);
    minimizers.emplace_back(0x222ULL, 40u, 0u);   // repeat kmer 0x222
    minimizers.emplace_back(0x444ULL, 77u, 1u);
    minimizers.emplace_back(0x222ULL, 100u, 1u);  // repeat kmer 0x222 again
    MinimizerHashmapFragmentIndex idx;
    idx.build(minimizers, 256);
    return idx;
}

TEST_CASE("vulkan index: GPU slot table resolves the same hits as CPU lookup",
          "[vulkan][index][phase-b]") {
    const auto idx = build_demo_index();
    const auto upload = idx_ns::build_gpu_minimizer_upload(idx);

    idx.for_each_table_entry([&](std::uint64_t kmer, std::uint64_t) {
        const auto cpu_hits = idx.lookup(kmer);  // span of encoded positions
        const std::uint32_t slot_idx = host_find_slot(upload, kmer);
        ASSERT(slot_idx != 0xffffffffu,
               fmt::format("GPU slot table missed kmer {}", kmer), kmer);
        const auto& slot = upload.slots[slot_idx];
        ASSERT(slot.pos_count == cpu_hits.size(),
               fmt::format("kmer {} count mismatch (gpu {} vs cpu {})",
                           kmer, slot.pos_count, cpu_hits.size()),
               slot.pos_count, cpu_hits.size());
        for (std::uint32_t j = 0; j < slot.pos_count; ++j) {
            ASSERT(upload.positions[slot.pos_offset + j] == cpu_hits[j],
                   fmt::format("kmer {} position {} mismatch", kmer, j), j);
        }
    });
    // A kmer not in the index must miss.
    ASSERT(host_find_slot(upload, 0xDEADBEEFULL) == 0xffffffffu,
           "absent kmer unexpectedly found in slot table", 0);
}

TEST_CASE("vulkan index: in-memory cache key is pointer-stable and content-keyed",
          "[vulkan][index][phase-b]") {
    auto idx_a = build_demo_index();
    auto idx_b = build_demo_index();  // identical content, different object

    const auto key_a = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::GpuIndexCacheIdentity{});
    const auto key_a2 = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::GpuIndexCacheIdentity{});
    const auto key_b = idx_ns::make_gpu_index_cache_key(idx_b, idx_ns::GpuIndexCacheIdentity{});

    ASSERT(key_a == key_a2, "same in-memory index must produce the same key", 0);
    ASSERT(!(key_a == key_b),
           "distinct in-memory index objects must produce distinct keys", 0);
}

TEST_CASE("vulkan index: distinct assembly identities do not share a cache key",
          "[vulkan][index][phase-b]") {
    // Two identical local segment indexes with different add_position_offset:
    // they must carry distinct explicit assembly_identity values and key apart.
    auto seg_lo = build_demo_index();
    auto seg_hi = build_demo_index();
    seg_hi.add_position_offset(1'000'000);  // global offset for a later segment

    const auto key_lo = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::GpuIndexCacheIdentity{.assembly_identity = 0xA11CEULL});
    const auto key_hi = idx_ns::make_gpu_index_cache_key(
        seg_hi, idx_ns::GpuIndexCacheIdentity{.assembly_identity = 0xB0B0ULL});

    ASSERT(!(key_lo == key_hi),
           "different assembly identities must not share a cache key", 0);
    // The same identity on the same index must be reproducible.
    const auto key_lo2 = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::GpuIndexCacheIdentity{.assembly_identity = 0xA11CEULL});
    ASSERT(key_lo == key_lo2, "assembly key must be reproducible", 0);

    idx_ns::GpuMinimizerIndexCacheKeyHash hasher;
    ASSERT(hasher(key_lo) != hasher(key_hi) || !(key_lo == key_hi),
           "distinct assembly keys collide on both equality and hash", 0);
}

TEST_CASE("vulkan index: resident upload + cache reuse (device path)",
          "[vulkan][index][phase-b]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto idx = build_demo_index();
    idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{64} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::GpuIndexCacheIdentity{});

    auto handle1 = cache.get_or_upload(runtime, idx, key, /*cache_enabled=*/true);
    ASSERT(handle1 != nullptr, "resident upload returned nullptr on a usable device", 0);
    ASSERT(handle1->target_len == idx.target_len, "resident target_len mismatch", 0);
    ASSERT(handle1->position_count == idx.positions.size(),
           "resident position_count mismatch", 0);

    auto handle2 = cache.get_or_upload(runtime, idx, key, /*cache_enabled=*/true);
    ASSERT(handle2.get() == handle1.get(),
           "cache must return the same resident handle for a repeated key", 0);

    cache.clear();
}

}  // namespace
