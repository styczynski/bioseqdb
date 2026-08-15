/**
 * @file gpu_minimizer_index_test.cpp
 * @brief Tests host GPU-index layouts, planning, cache identity, and decline reporting.
 */

#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

import bioseq.common.logging;
import bioseq.sequences.kmers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;

import bioseq.tests.unit.indexing.gpu_minimizer_index_support;

using minimap_sketch_data = bioseq::indexing::minimap_sketch_data;
using bioseq::indexing::minimizer_hashmap_fragment_index;
namespace idx_ns = bioseq::indexing;
using namespace bioseq::tests::unit::indexing::gpu_minimizer_index_test_support;

namespace {

TEST_CASE("vulkan index: GPU slot table resolves the same hits as CPU lookup",
          "[component:bioseq][layer:unit][feature:index]") {
    const auto idx = build_demo_index();
    const auto upload = idx_ns::build_gpu_minimizer_upload(idx);

    idx.for_each_table_entry([&idx, &upload](std::uint64_t kmer, std::uint64_t) {
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

TEST_CASE("vulkan index: XXL upper-bound plan is 2^27 slots at <=70% load",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t positions = 93'308'896u;
    constexpr std::uint64_t expected_capacity = std::uint64_t{1} << 27;
    const std::uint64_t capacity = idx_ns::gpu_slot_capacity(positions);
    REQUIRE(capacity == expected_capacity);
    REQUIRE(idx_ns::gpu_index_load_ppm(positions, capacity) <= 700'000u);
    REQUIRE(capacity * sizeof(idx_ns::gpu_minimizer_slot) == 2'147'483'648u);
    REQUIRE(positions * sizeof(std::uint64_t) == 746'471'168u);
    REQUIRE(capacity * sizeof(idx_ns::gpu_minimizer_slot)
            + positions * sizeof(std::uint64_t) == 2'893'954'816u);

    constexpr std::uint64_t max_distinct_for_2p27 =
        ((std::uint64_t{1} << 27) * 7u) / 10u;
    REQUIRE(idx_ns::gpu_slot_capacity(max_distinct_for_2p27)
            == expected_capacity);
    REQUIRE(idx_ns::gpu_slot_capacity(max_distinct_for_2p27 + 1u)
            == (std::uint64_t{1} << 28));
}

TEST_CASE("vulkan index: storage-range preflight accepts equality and rejects +1/unknown",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t limit = 4'294'967'295u;
    REQUIRE(idx_ns::gpu_index_storage_ranges_fit(limit, 8u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(limit + 1u, 8u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(16u, limit + 1u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(16u, 8u, 0u));
}

TEST_CASE("vulkan index: direct fill preserves collision and wraparound parity",
          "[component:bioseq][layer:unit][feature:index]") {
    std::vector<std::uint64_t> colliding_hashes;
    for (std::uint64_t hash = 1u; colliding_hashes.size() < 3u; ++hash) {
        if ((idx_ns::splitmix64(hash) & 7u) == 7u) {
            colliding_hashes.push_back(hash);
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {colliding_hashes[0], 11u},
        {colliding_hashes[1], 22u},
        {colliding_hashes[2], 33u},
    };
    minimizer_hashmap_fragment_index idx;
    idx.build_from_pairs(pairs, 100u);
    const std::uint32_t capacity = idx_ns::gpu_slot_capacity(idx.table_size());
    REQUIRE(capacity == 8u);
    std::vector<idx_ns::gpu_minimizer_slot> final_slots(capacity);
    std::vector<std::uint64_t> final_positions(idx.positions.size());
    const auto stats = idx_ns::direct_fill_gpu_minimizer_index(
        idx, final_slots, final_positions);
    REQUIRE(stats.unique_key_count == 3u);
    REQUIRE(stats.slot_capacity == 8u);
    REQUIRE(stats.load_ppm <= 700'000u);
    REQUIRE(stats.max_probe == 3u);
    REQUIRE(final_positions == idx.positions);
    for (const std::uint64_t hash : colliding_hashes) {
        const std::uint32_t slot = host_find_slot(
            std::span<const idx_ns::gpu_minimizer_slot>{final_slots},
            capacity - 1u, hash);
        REQUIRE(slot != 0xffffffffu);
        const auto cpu = idx.lookup(hash);
        REQUIRE(final_slots[slot].pos_count == cpu.size());
        REQUIRE(final_positions[final_slots[slot].pos_offset] == cpu.front());
    }
    REQUIRE(final_slots[7].pos_count != 0u);
    REQUIRE(final_slots[0].pos_count != 0u);
    REQUIRE(final_slots[1].pos_count != 0u);
    REQUIRE(idx_ns::gpu_index_upload_peak_bytes(idx) == 0u);
}

TEST_CASE("vulkan index: sorted-dense direct fill preserves shader lookup parity",
          "[component:bioseq][layer:unit][feature:index]") {
    auto idx = build_demo_index();
    std::vector<idx_ns::gpu_minimizer_slot> final_slots(idx.table_size());
    std::vector<std::uint64_t> final_positions(idx.positions.size());
    const auto stats = idx_ns::direct_fill_gpu_minimizer_index_sorted_dense(
        idx, final_slots, final_positions);

    REQUIRE(stats.unique_key_count == idx.table_size());
    REQUIRE(stats.slot_capacity == idx.table_size());
    REQUIRE(stats.load_ppm == 1'000'000u);
    REQUIRE(stats.max_probe == 0u);
    REQUIRE(final_positions == idx.positions);
    REQUIRE(std::is_sorted(
        final_slots.begin(), final_slots.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.hash < rhs.hash;
        }));
    for (const std::uint64_t hash : {0x111ULL, 0x222ULL, 0x333ULL, 0x444ULL}) {
        const std::uint32_t slot = host_find_sorted_dense_slot(
            final_slots, hash);
        REQUIRE(slot != 0xffffffffu);
        const auto cpu = idx.lookup(hash);
        REQUIRE(final_slots[slot].pos_count == cpu.size());
        REQUIRE(std::equal(
            cpu.begin(), cpu.end(),
            final_positions.begin() + final_slots[slot].pos_offset));
    }
    REQUIRE(host_find_sorted_dense_slot(final_slots, 0x999ULL)
            == 0xffffffffu);
}

TEST_CASE("vulkan index: compact plan removes the observed XXL power-of-two cliff",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t unique_keys = 70'249'364u;
    constexpr std::uint64_t positions = 93'308'896u;
    const auto open = idx_ns::plan_gpu_index_materialization(
        unique_keys, positions,
        idx_ns::gpu_index_representation::open_addressed_70);
    const auto dense = idx_ns::plan_gpu_index_materialization(
        unique_keys, positions,
        idx_ns::gpu_index_representation::source_direct_sorted_dense);

    REQUIRE(open.slot_capacity == 134'217'728u);
    REQUIRE(open.slot_bytes == 2'147'483'648u);
    REQUIRE(open.position_bytes == 746'471'168u);
    REQUIRE(open.resident_bytes == 2'893'954'816u);
    REQUIRE(dense.slot_capacity == unique_keys);
    REQUIRE(dense.slot_bytes == 1'123'989'824u);
    REQUIRE(dense.position_bytes == open.position_bytes);
    REQUIRE(dense.resident_bytes == 1'870'460'992u);
    REQUIRE(dense.resident_bytes < open.resident_bytes);
    REQUIRE(dense.load_ppm == 1'000'000u);
}

TEST_CASE("vulkan index: in-memory cache key is pointer-stable and content-keyed",
          "[component:bioseq][layer:unit][feature:index]") {
    auto idx_a = build_demo_index();
    auto idx_b = build_demo_index();  // identical content, different object

    const auto key_a = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::gpu_index_cache_identity{});
    const auto key_a2 = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::gpu_index_cache_identity{});
    const auto key_b = idx_ns::make_gpu_index_cache_key(idx_b, idx_ns::gpu_index_cache_identity{});

    ASSERT(key_a == key_a2, "same in-memory index must produce the same key", 0);
    ASSERT(!(key_a == key_b),
           "distinct in-memory index objects must produce distinct keys", 0);
}

TEST_CASE("vulkan index: distinct assembly identities do not share a cache key",
          "[component:bioseq][layer:unit][feature:index]") {
    // Two identical local segment indexes with different add_position_offset:
    // they must carry distinct explicit assembly_identity values and key apart.
    auto seg_lo = build_demo_index();
    auto seg_hi = build_demo_index();
    seg_hi.add_position_offset(1'000'000);  // global offset for a later segment

    const auto key_lo = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::gpu_index_cache_identity{.assembly_identity = 0xA11CEULL});
    const auto key_hi = idx_ns::make_gpu_index_cache_key(
        seg_hi, idx_ns::gpu_index_cache_identity{.assembly_identity = 0xB0B0ULL});

    ASSERT(!(key_lo == key_hi),
           "different assembly identities must not share a cache key", 0);
    // The same identity on the same index must be reproducible.
    const auto key_lo2 = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::gpu_index_cache_identity{.assembly_identity = 0xA11CEULL});
    ASSERT(key_lo == key_lo2, "assembly key must be reproducible", 0);

    idx_ns::gpu_minimizer_index_cache_key_hash hasher;
    ASSERT(hasher(key_lo) != hasher(key_hi) || !(key_lo == key_hi),
           "distinct assembly keys collide on both equality and hash", 0);
}

TEST_CASE("vulkan index: query-gate cache identity compares canonical words after digest",
          "[component:bioseq][layer:unit][feature:index]") {
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{std::uint64_t{64} << 20};
    auto words_a_mut = std::make_shared<std::vector<std::uint32_t>>(
        bioseq::gpu::vulkan::k_gpu_query_gate_word_count, 0u);
    (*words_a_mut)[0] = 0x1u;
    (*words_a_mut)[1] = 0x20u;
    auto words_b_mut =
        std::make_shared<std::vector<std::uint32_t>>(*words_a_mut);
    (*words_b_mut)[2] = 0x301u;
    const std::shared_ptr<const std::vector<std::uint32_t>> words_a =
        std::move(words_a_mut);
    const std::shared_ptr<const std::vector<std::uint32_t>> words_b =
        std::move(words_b_mut);
    const cache_type::gate_identity first{
        .enabled = true,
        .word_count = bioseq::gpu::vulkan::k_gpu_query_gate_word_count,
        .mask = bioseq::gpu::vulkan::k_gpu_query_gate_mask,
        .digest_lo = 0x1111222233334444ULL,
        .digest_hi = 0x5555666677778888ULL,
        .canonical_words = words_a,
    };
    auto forced_collision = first;
    forced_collision.canonical_words = words_b;
    REQUIRE(cache.bind_gate_once(first).has_value());
    const auto rejected = cache.bind_gate_once(forced_collision);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == cache_type::gate_bind_error::identity_mismatch);

    cache_type zero_cache{std::uint64_t{64} << 20};
    const auto zero_words =
        std::make_shared<const std::vector<std::uint32_t>>(
            bioseq::gpu::vulkan::k_gpu_query_gate_word_count, 0u);
    REQUIRE(zero_cache.bind_gate_once(cache_type::gate_identity{
        .enabled = true,
        .word_count = bioseq::gpu::vulkan::k_gpu_query_gate_word_count,
        .mask = bioseq::gpu::vulkan::k_gpu_query_gate_mask,
        .digest_lo = 9u,
        .digest_hi = 10u,
        .canonical_words = zero_words,
    }).has_value());
    const auto disabled = zero_cache.bind_gate_once(cache_type::gate_identity{});
    REQUIRE_FALSE(disabled.has_value());
    REQUIRE(disabled.error() == cache_type::gate_bind_error::identity_mismatch);
}

TEST_CASE("vulkan index: every cache API rejects an unbound gated key",
          "[component:bioseq][layer:unit][feature:index]") {
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{std::uint64_t{64} << 20};
    idx_ns::gpu_minimizer_index_cache_key gated_key{};
    gated_key.gate_enabled = true;
    gated_key.gate_word_count = 1u;
    gated_key.gate_mask = 31u;
    gated_key.gate_digest_lo = 11u;
    gated_key.gate_digest_hi = 12u;

    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    auto idx = build_demo_index();
    REQUIRE_THROWS_AS(
        cache.get_or_upload(
            runtime, idx, gated_key, /*cache_enabled=*/false),
        bioseq::gpu::vulkan::vulkan_runtime_error);
    REQUIRE_THROWS_AS(
        cache.insert_upload(
            runtime, idx_ns::gpu_minimizer_index_upload{}, gated_key,
            /*cache_enabled=*/false),
        bioseq::gpu::vulkan::vulkan_runtime_error);
    auto empty_handle = std::make_shared<idx_ns::gpu_resident_minimizer_index>();
    const auto built_insert = cache.insert_built(gated_key, empty_handle);
    REQUIRE_FALSE(built_insert.has_value());
    REQUIRE(built_insert.error()
            == cache_type::built_insert_error::gate_identity_mismatch);

    const auto truncated_words =
        std::make_shared<const std::vector<std::uint32_t>>(1u, 1u);
    const auto malformed = cache.bind_gate_once(cache_type::gate_identity{
        .enabled = true,
        .word_count = 1u,
        .mask = 31u,
        .digest_lo = 1u,
        .digest_hi = 2u,
        .canonical_words = truncated_words,
    });
    REQUIRE_FALSE(malformed.has_value());
    REQUIRE(malformed.error() == cache_type::gate_bind_error::invalid_identity);
}

TEST_CASE("vulkan index: typed upload decline preserves every predicate and admission field",
          "[component:bioseq][layer:unit][feature:index]") {
    const bioseq::parallelism::memory_admission_failure admission{
        .reason = bioseq::parallelism::memory_admission_failure_reason::
            budget_exhausted,
        .requested_bytes = 701u,
        .reserved_bytes = 211u,
        .budget_ceiling_bytes = 503u,
        .allocatable_after_reservations_bytes = 292u,
        .process_footprint_bytes = 401u,
        .system_available_bytes = 601u,
        .system_total_bytes = 809u,
        .process_soft_cap_bytes = 307u,
    };
    for (const auto reason : {
            idx_ns::gpu_index_upload_decline_reason::
                resident_memory_admission_denied,
            idx_ns::gpu_index_upload_decline_reason::
                upload_peak_memory_admission_denied}) {
        const idx_ns::gpu_index_upload_decline decline{
            .reason = reason,
            .resident_bytes = 101u,
            .upload_peak_bytes = 202u,
            .slot_bytes = 303u,
            .position_bytes = 404u,
            .unique_key_count = 505u,
            .slot_capacity = 606u,
            .staging_bytes = 0u,
            .actual_allocation_bytes = 707u,
            .fully_touched_bytes = 707u,
            .reflected_footprint_delta_bytes = 808u,
            .load_ppm = 699'999u,
            .max_probe = 9u,
            .materialization_permit_released = true,
            .process_footprint_reflected = true,
            .admission = admission,
        };
        const std::string formatted =
            idx_ns::format_gpu_index_upload_decline(decline);
        REQUIRE(formatted.find(idx_ns::gpu_index_upload_decline_reason_name(reason))
                != std::string::npos);
        REQUIRE(formatted.find("admission_reason=budget_exhausted")
                != std::string::npos);
        REQUIRE(formatted.find("required_bytes=701") != std::string::npos);
        REQUIRE(formatted.find("reserved_bytes=211") != std::string::npos);
        REQUIRE(formatted.find("budget_ceiling_bytes=503") != std::string::npos);
        REQUIRE(formatted.find("process_footprint_bytes=401")
                != std::string::npos);
        REQUIRE(formatted.find("system_available_bytes=601")
                != std::string::npos);
        REQUIRE(formatted.find("system_total_bytes=809") != std::string::npos);
        REQUIRE(formatted.find("process_soft_cap_bytes=307")
                != std::string::npos);
        REQUIRE(formatted.find("resident_bytes=101") != std::string::npos);
        REQUIRE(formatted.find("upload_peak_bytes=202") != std::string::npos);
        REQUIRE(formatted.find("slot_bytes=303") != std::string::npos);
        REQUIRE(formatted.find("position_bytes=404") != std::string::npos);
        REQUIRE(formatted.find("unique_key_count=505") != std::string::npos);
        REQUIRE(formatted.find("slot_capacity=606") != std::string::npos);
        REQUIRE(formatted.find("load_ppm=699999") != std::string::npos);
        REQUIRE(formatted.find("max_probe=9") != std::string::npos);
        REQUIRE(formatted.find("representation=open_addressed_70")
                != std::string::npos);
        REQUIRE(formatted.find("staging_bytes=0") != std::string::npos);
        REQUIRE(formatted.find("actual_allocation_bytes=707")
                != std::string::npos);
        REQUIRE(formatted.find("fully_touched_bytes=707")
                != std::string::npos);
        REQUIRE(formatted.find("reflected_footprint_delta_bytes=808")
                != std::string::npos);
        REQUIRE(formatted.find("materialization_permit_released=true")
                != std::string::npos);
        REQUIRE(formatted.find("process_footprint_reflected=true")
                != std::string::npos);
    }

    for (const auto reason : {
            idx_ns::gpu_index_upload_decline_reason::device_abi_overflow,
            idx_ns::gpu_index_upload_decline_reason::
                non_host_visible_device_memory}) {
        const idx_ns::gpu_index_upload_decline decline{
            .reason = reason,
            .resident_bytes = 11u,
            .upload_peak_bytes = 22u,
            .slot_bytes = 33u,
            .position_bytes = 44u,
        };
        const std::string formatted =
            idx_ns::format_gpu_index_upload_decline(decline);
        REQUIRE(formatted.find(idx_ns::gpu_index_upload_decline_reason_name(reason))
                != std::string::npos);
        REQUIRE(formatted.find("slot_bytes=33") != std::string::npos);
        REQUIRE(formatted.find("position_bytes=44") != std::string::npos);
    }
}

} // namespace
