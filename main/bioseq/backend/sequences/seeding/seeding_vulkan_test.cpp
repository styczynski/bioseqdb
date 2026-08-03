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
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

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
                                            std::uint32_t, std::uint8_t,
                                            std::uint8_t, std::uint16_t>>
anchor_tuples(const ch::ArenaSlab<Anchor>& slab) {
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint32_t,
                           std::uint8_t, std::uint8_t, std::uint16_t>> out;
    for (std::size_t i = 0; i < slab.size(); ++i) {
        const Anchor& a = slab.data()[i];
        out.emplace_back(a.ref_position, a.query_position, a.is_reverse,
                         a.query_span, a.flags, a.segment_id);
    }
    return out;
}

[[nodiscard]] static bool nondecreasing_ref_bits(const ch::ArenaSlab<Anchor>& slab) {
    for (std::size_t i = 1; i < slab.size(); ++i) {
        if (slab.data()[i].ref_bits() < slab.data()[i - 1].ref_bits()) return false;
    }
    return true;
}

TEST_CASE("vulkan seeding: scalable sort plan boundary and XXL arithmetic",
          "[vulkan][seeding][phase-b][sort-plan]") {
    constexpr std::uint64_t max_range =
        (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(vk::seed_anchor_bitonic_capacity(
        /*max_invocations=*/1024u, /*max_size_x=*/1024u,
        /*shared_bytes=*/32u * 1024u) == 1024u);
    REQUIRE(vk::seed_anchor_bitonic_capacity(
        /*max_invocations=*/1024u, /*max_size_x=*/512u,
        /*shared_bytes=*/32u * 1024u) == 512u);
    REQUIRE(vk::seed_anchor_bitonic_capacity(
        /*max_invocations=*/512u, /*max_size_x=*/1024u,
        /*shared_bytes=*/32u * 1024u) == 512u);
    REQUIRE(vk::seed_anchor_bitonic_capacity(
        /*max_invocations=*/1024u, /*max_size_x=*/1024u,
        /*shared_bytes=*/14'335u) == 256u);
    const auto at_capacity = vk::make_seed_anchor_sort_plan(
        1024u, 500'000'000u, 0u, 1024u, 65'535u,
        256u, 256u, max_range);
    REQUIRE(at_capacity.valid());
    REQUIRE(at_capacity.backend == vk::SeedAnchorSortBackend::bitonic);
    REQUIRE(at_capacity.anchor_bytes == 24'576u);
    REQUIRE(at_capacity.key_bytes == 8'192u);
    REQUIRE(at_capacity.device_sort_bytes == 32'776u);
    REQUIRE(at_capacity.host_result_bytes == 24'576u);
    REQUIRE(at_capacity.transient_bytes == 57'352u);

    const auto above_capacity = vk::make_seed_anchor_sort_plan(
        1025u, 500'000'000u, 0u, 1024u, 65'535u,
        256u, 256u, max_range);
    REQUIRE(above_capacity.valid());
    REQUIRE(above_capacity.backend == vk::SeedAnchorSortBackend::radix);
    REQUIRE(above_capacity.num_workgroups == 3u);
    REQUIRE(above_capacity.elements_per_workgroup == 342u);
    REQUIRE(above_capacity.histogram_bytes == 3'072u);
    REQUIRE(above_capacity.device_sort_bytes == 68'672u);
    REQUIRE(above_capacity.host_result_bytes == 24'600u);
    REQUIRE(above_capacity.transient_bytes == 93'272u);

    const auto report87 = vk::make_seed_anchor_sort_plan(
        1'520'944u, 500'000'000u, 0u, 1024u, 65'535u,
        256u, 256u, max_range);
    REQUIRE(report87.valid());
    REQUIRE(report87.backend == vk::SeedAnchorSortBackend::radix);
    REQUIRE(report87.target_ref_bits == 29u);
    REQUIRE(report87.radix_pass_count == 4u);
    REQUIRE(report87.num_workgroups == 2'971u);
    REQUIRE(report87.elements_per_workgroup == 512u);
    REQUIRE(report87.anchor_bytes == 36'502'656u);
    REQUIRE(report87.key_bytes == 12'167'552u);
    REQUIRE(report87.histogram_bytes == 3'042'304u);
    REQUIRE(report87.device_sort_bytes == 100'382'720u);
    REQUIRE(report87.host_result_bytes == 36'502'656u);
    REQUIRE(report87.transient_bytes == 136'885'376u);

    const vk::VulkanSeedBufferAllocationRequirements requirements{
        .scratch_bytes = 4u,
        .prefix_bytes = 404u,
        .anchors_bytes = report87.anchor_bytes,
        .anchor_keys_bytes = report87.key_bytes,
        .anchors_scratch_bytes = report87.anchor_bytes,
        .anchor_keys_scratch_bytes = report87.key_bytes,
        .histogram_bytes = report87.histogram_bytes,
    };
    const auto no_slab_growth = vk::make_vulkan_seed_reservation_plan(
        requirements, 404u, report87.host_result_bytes, 0u);
    REQUIRE(no_slab_growth.arithmetic_valid);
    REQUIRE(no_slab_growth.vulkan_allocation_requirement_bytes
            == 100'383'128u);
    REQUIRE(no_slab_growth.total_bytes == 136'886'188u);
    REQUIRE(vk::vulkan_seed_reservation_fits(
        no_slab_growth, no_slab_growth.total_bytes));
    REQUIRE_FALSE(vk::vulkan_seed_reservation_fits(
        no_slab_growth, no_slab_growth.total_bytes - 1u));

    const auto with_slab_growth = vk::make_vulkan_seed_reservation_plan(
        requirements,
        404u,
        report87.host_result_bytes,
        report87.anchor_bytes);
    REQUIRE(with_slab_growth.arithmetic_valid);
    REQUIRE(with_slab_growth.total_bytes == 173'388'844u);
    REQUIRE(with_slab_growth.anchors_out_growth_bytes == 36'502'656u);
}

TEST_CASE("vulkan seeding: canonical anchor proof normalizes ref ties and digests explicit fields",
          "[vulkan][seeding][anchor-proof]") {
    std::vector<vk::SeedAnchorWire> anchors{
        {.ref_position = 7u, .query_position = 30u, .is_reverse = 0u,
         .query_span = 15u, .flags = 0u, .segment_id = 0u},
        {.ref_position = 7u, .query_position = 10u, .is_reverse = 0u,
         .query_span = 15u, .flags = 0u, .segment_id = 0u},
        {.ref_position = 9u, .query_position = 20u, .is_reverse = 0u,
         .query_span = 15u, .flags = 0u, .segment_id = 0u},
    };
    const auto normalized = vk::normalize_seed_anchor_equal_ref_ties(
        std::as_writable_bytes(std::span{anchors}), anchors.size());
    REQUIRE(normalized.gpu_ref_order_checked);
    REQUIRE(normalized.gpu_ref_order_accepted);
    REQUIRE(normalized.canonical_tie_run_count == 1u);
    REQUIRE(normalized.canonical_tie_anchor_count == 2u);
    REQUIRE(anchors[0].query_position == 10u);
    REQUIRE(anchors[1].query_position == 30u);

    const auto proof = vk::make_seed_anchor_canonical_proof(
        std::as_bytes(std::span{anchors}), anchors.size());
    REQUIRE(proof.anchor_count == anchors.size());
    REQUIRE(proof.canonical_order_checked);
    REQUIRE(proof.canonical_order_accepted);

    auto mutated = anchors;
    mutated[1].flags = 1u;
    const auto mutated_proof = vk::make_seed_anchor_canonical_proof(
        std::as_bytes(std::span{mutated}), mutated.size());
    REQUIRE(mutated_proof.canonical_order_accepted);
    REQUIRE((mutated_proof.digest_lo != proof.digest_lo
             || mutated_proof.digest_hi != proof.digest_hi));

    auto ref_disordered = anchors;
    std::swap(ref_disordered.front(), ref_disordered.back());
    const auto rejected = vk::normalize_seed_anchor_equal_ref_ties(
        std::as_writable_bytes(std::span{ref_disordered}),
        ref_disordered.size());
    REQUIRE(rejected.gpu_ref_order_checked);
    REQUIRE_FALSE(rejected.gpu_ref_order_accepted);

    const auto empty = vk::make_seed_anchor_canonical_proof({}, 0u);
    REQUIRE(empty.anchor_count == 0u);
    REQUIRE(empty.digest_lo == vk::kSeedAnchorDigestSeedLo);
    REQUIRE(empty.digest_hi == vk::kSeedAnchorDigestSeedHi);
    REQUIRE(empty.canonical_order_checked);
    REQUIRE(empty.canonical_order_accepted);
}

TEST_CASE("vulkan seeding: mapped sorted-dense two-pass prefix is exact and immutable",
          "[vulkan][seeding][mapped-prefix][authority]"
          "[vulkan][seeding][phase-b][mapped-prefix-authority]") {
    std::vector<idx_ns::GpuMinimizerSlot> controls{
        {.hash = 0x10u, .pos_offset = 0u, .pos_count = 2u},
        {.hash = 0x20u, .pos_offset = 2u, .pos_count = 1u},
    };
    std::vector<std::uint64_t> positions{
        enc(3u, 0u), enc(5u, 1u), enc(8u, 0u)};
    std::uint64_t checksum = idx_ns::kGpuSourceDirectChecksumSeed;
    for (const auto& control : controls) {
        checksum = idx_ns::gpu_source_direct_checksum_word(
            checksum, control.hash);
        checksum = idx_ns::gpu_source_direct_checksum_word(
            checksum, control.pos_offset);
        for (std::uint64_t i = control.pos_offset;
             i < control.pos_offset + control.pos_count; ++i) {
            checksum = idx_ns::gpu_source_direct_checksum_word(
                checksum, positions[static_cast<std::size_t>(i)]);
        }
        checksum = idx_ns::gpu_source_direct_checksum_word(
            checksum, control.pos_count);
    }
    const auto accepted = vk::validate_mapped_sorted_dense_payload(
        controls, positions, 100u, checksum);
    REQUIRE(accepted.canonical_order_checked);
    REQUIRE(accepted.accepted);
    REQUIRE(accepted.covered_position_count == positions.size());

    // Both present hashes share their low four bits. The mapped route must use
    // sorted-dense binary search, handle a normal miss, and count duplicate
    // query hashes exactly.
    const std::vector<MinimapSketchData> query{
        MinimapSketchData{0x10u, 1u, 0u},
        MinimapSketchData{0x20u, 2u, 0u},
        MinimapSketchData{0x30u, 3u, 0u},
        MinimapSketchData{0x10u, 4u, 1u},
    };
    const auto query_span = std::span<const MinimapSketchData>{query};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> oracle_pairs{
        {0x10u, positions[0]},
        {0x10u, positions[1]},
        {0x20u, positions[2]},
    };
    MinimizerHashmapFragmentIndex oracle;
    oracle.build_from_pairs(oracle_pairs, 100u);

    const auto mapped_plan = vk::count_seed_prefix_plan(
        query_span,
        vk::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/500);
    const auto oracle_plan = vk::count_seed_prefix_plan(
        query_span, oracle, /*max_occ=*/500);
    REQUIRE(mapped_plan.prefix_len == query.size() + 1u);
    REQUIRE(mapped_plan.exact_anchor_count == 5u);
    REQUIRE(mapped_plan.exact_anchor_count
            == oracle_plan.exact_anchor_count);
    REQUIRE(mapped_plan.mapped_lookup_count == query.size());
    REQUIRE(mapped_plan.mapped_comparison_count >= query.size());

    const auto mapped_prefix =
        vk::build_seed_prefix_counts_after_admission(
            query_span,
            vk::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/500,
            mapped_plan);
    const auto oracle_prefix =
        vk::build_seed_prefix_counts_after_admission(
            query_span, oracle, /*max_occ=*/500, oracle_plan);
    REQUIRE((mapped_prefix.prefix
             == std::vector<std::uint32_t>{0u, 2u, 3u, 3u, 5u}));
    REQUIRE(mapped_prefix.prefix == oracle_prefix);
    REQUIRE(mapped_prefix.mapped_lookup_count == query.size());
    REQUIRE(mapped_prefix.mapped_comparison_count >= query.size());

    // A nonempty resident paired with an empty minimizer span is still a
    // source-direct operation, not the generic empty-query shortcut.  Both
    // calls reconstruct and validate the complete mapped payload; the absence
    // of query hashes legitimately produces zero binary-search activity.
    const std::span<const MinimapSketchData> empty_query{};
    const auto empty_mapped_plan = vk::count_seed_prefix_plan(
        empty_query,
        vk::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/500);
    const auto empty_mapped_prefix =
        vk::build_seed_prefix_counts_after_admission(
            empty_query,
            vk::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/500,
            empty_mapped_plan);
    REQUIRE(empty_mapped_plan.prefix_len == 1u);
    REQUIRE(empty_mapped_plan.exact_anchor_count == 0u);
    REQUIRE(empty_mapped_plan.mapped_lookup_count == 0u);
    REQUIRE(empty_mapped_plan.mapped_comparison_count == 0u);
    REQUIRE(empty_mapped_prefix.prefix
            == std::vector<std::uint32_t>{0u});
    REQUIRE(empty_mapped_prefix.mapped_lookup_count == 0u);
    REQUIRE(empty_mapped_prefix.mapped_comparison_count == 0u);

    const auto limited_plan = vk::count_seed_prefix_plan(
        query_span,
        vk::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/1);
    const auto limited_prefix =
        vk::build_seed_prefix_counts_after_admission(
            query_span,
            vk::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/1,
            limited_plan);
    REQUIRE(limited_plan.exact_anchor_count == 1u);
    REQUIRE((limited_prefix.prefix
             == std::vector<std::uint32_t>{0u, 0u, 1u, 1u, 1u}));

    const auto disabled_plan = vk::count_seed_prefix_plan(
        query_span,
        vk::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/-1);
    const auto disabled_prefix =
        vk::build_seed_prefix_counts_after_admission(
            query_span,
            vk::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/-1,
            disabled_plan);
    REQUIRE(disabled_plan.exact_anchor_count == 0u);
    REQUIRE(disabled_plan.mapped_lookup_count == 0u);
    REQUIRE(disabled_prefix.prefix
            == std::vector<std::uint32_t>(query.size() + 1u, 0u));

    auto divergent_plan = mapped_plan;
    ++divergent_plan.exact_anchor_count;
    REQUIRE_THROWS_AS(
        vk::build_seed_prefix_counts_after_admission(
            query_span,
            vk::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/500,
            divergent_plan),
        vk::MappedPrefixAuthorityInvalid);

    auto mutated_positions = positions;
    mutated_positions[1] ^= 2u;
    REQUIRE_FALSE(vk::validate_mapped_sorted_dense_payload(
        controls, mutated_positions, 100u, checksum).accepted);

    auto mutated_controls = controls;
    mutated_controls[1].pos_offset = 1u;
    REQUIRE_FALSE(vk::validate_mapped_sorted_dense_payload(
        mutated_controls, positions, 100u, checksum).accepted);

    // The second pass revalidates the whole mapped payload, so mutation after
    // the count receipt fails before any prefix can be published.
    const auto mutated_second_pass = [
        &controls,
        &mutated_positions,
        checksum,
        query_span,
        mapped_plan] {
        auto second_view = vk::validated_mapped_sorted_dense_prefix_view(
            controls, mutated_positions, 100u, checksum);
        static_cast<void>(vk::build_seed_prefix_counts_after_admission(
            query_span, std::move(second_view), /*max_occ=*/500,
            mapped_plan));
    };
    REQUIRE_THROWS_AS(mutated_second_pass(),
                      vk::MappedPrefixAuthorityInvalid);

    constexpr std::uint64_t u32_max =
        (std::numeric_limits<std::uint32_t>::max)();
    REQUIRE(vk::checked_seed_prefix_length(u32_max - 1u) == u32_max);
    REQUIRE(vk::checked_seed_anchor_total(u32_max - 1u, 1u) == u32_max);
    try {
        static_cast<void>(vk::checked_seed_prefix_length(u32_max));
        FAIL("u32+1 prefix length was accepted");
    } catch (const vk::SeedLookupTooLarge& error) {
        REQUIRE(error.kind == vk::SeedLookupOverflowKind::query_prefix_length);
        REQUIRE(error.observed == u32_max + 1u);
    }
    try {
        static_cast<void>(vk::checked_seed_anchor_total(u32_max, 1u));
        FAIL("u32+1 exact anchor total was accepted");
    } catch (const vk::SeedLookupTooLarge& error) {
        REQUIRE(error.kind == vk::SeedLookupOverflowKind::exact_anchor_count);
        REQUIRE(error.observed == u32_max + 1u);
    }
}

TEST_CASE("vulkan seeding: sort plan proves local-key offset order and typed failures",
          "[vulkan][seeding][phase-b][sort-plan][decline]") {
    constexpr std::uint64_t max_range =
        (std::numeric_limits<std::uint64_t>::max)();
    const auto offset_plan = vk::make_seed_anchor_sort_plan(
        1025u, std::uint64_t{1} << 29u, std::uint64_t{1} << 32u,
        1024u, 65'535u, 256u, 256u, max_range);
    REQUIRE(offset_plan.valid());
    REQUIRE(offset_plan.target_ref_bits == 29u);
    REQUIRE(offset_plan.target_position_offset == (std::uint64_t{1} << 32u));
    REQUIRE(offset_plan.max_local_ref_position
            == (std::uint64_t{1} << 29u) - 1u);
    REQUIRE(offset_plan.max_absolute_ref_position
            == (std::uint64_t{1} << 32u)
                + (std::uint64_t{1} << 29u) - 1u);
    // Adding the one segment offset preserves local ordering; the shader can
    // sort local keys while the surrounding segment contract owns rebasing.
    constexpr std::uint64_t lhs = 17u;
    constexpr std::uint64_t rhs = 99u;
    REQUIRE(lhs < rhs);
    REQUIRE(lhs + offset_plan.target_position_offset
            < rhs + offset_plan.target_position_offset);

    const auto count_failure = vk::make_seed_anchor_sort_plan(
        std::uint64_t{1} + (std::numeric_limits<std::uint32_t>::max)(),
        1u, 0u, 1024u, 65'535u, 256u, 256u, max_range);
    REQUIRE(count_failure.failure
            == vk::SeedAnchorSortPlanFailure::anchor_count_exceeds_u32);
    const auto coordinate_failure = vk::make_seed_anchor_sort_plan(
        1025u, 1u, std::uint64_t{1} << 63u,
        1024u, 65'535u, 256u, 256u, max_range);
    REQUIRE(coordinate_failure.failure
            == vk::SeedAnchorSortPlanFailure::target_coordinate_invalid);
    const auto dispatch_failure = vk::make_seed_anchor_sort_plan(
        1025u, 1000u, 0u, 1024u, 0u, 256u, 256u, max_range);
    REQUIRE(dispatch_failure.failure
            == vk::SeedAnchorSortPlanFailure::dispatch_limit_unavailable);
    const auto geometry_failure = vk::make_seed_anchor_sort_plan(
        1025u, 1000u, 0u, 1024u, 65'535u, 128u, 128u, max_range);
    REQUIRE(geometry_failure.failure
            == vk::SeedAnchorSortPlanFailure::
                radix_workgroup_geometry_unsupported);
    const auto range_ok = vk::make_seed_anchor_sort_plan(
        1024u, 1000u, 0u, 1024u, 65'535u,
        256u, 256u, 24'576u);
    REQUIRE(range_ok.valid());
    const auto range_failure = vk::make_seed_anchor_sort_plan(
        1024u, 1000u, 0u, 1024u, 65'535u,
        256u, 256u, 24'575u);
    REQUIRE(range_failure.failure
            == vk::SeedAnchorSortPlanFailure::storage_buffer_range_exceeded);
    REQUIRE_FALSE(vk::seed_radix_result_in_scratch(4u));
    REQUIRE(vk::seed_radix_result_in_scratch(3u));
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
    ::bioseq::simd::collect_seed_hits<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        idx, query_len, max_occ, kmer_size, cpu_slab);

    idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{64} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::GpuIndexCacheIdentity{});
    auto resident = cache.get_or_upload(runtime, idx, key, /*cache_enabled=*/true);
    ASSERT(resident != nullptr, "resident index upload failed", 0);

    auto gpu_slab = ch::make_arena_slab<Anchor>();
    gpu_slab.reserve(64);
    vk::VulkanSeedLookupDecline decline{};
    const bool ok = vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt, idx, resident, /*vulkan_enabled=*/true,
        query_len, max_occ, kmer_size, gpu_slab,
        /*fused_frontend=*/false, &decline);
    ASSERT(ok, "collect_seed_hits_vulkan returned false on a usable device", 0);

    ASSERT(cpu_slab.size() == gpu_slab.size(),
           fmt::format("anchor count mismatch (cpu {} vs gpu {})",
                       cpu_slab.size(), gpu_slab.size()),
           cpu_slab.size(), gpu_slab.size());
    ASSERT(cpu_slab.size() == 5,
           fmt::format("expected 5 anchors, got {}", cpu_slab.size()),
           cpu_slab.size());
    ASSERT(nondecreasing_ref_bits(gpu_slab), "GPU anchors are not ref_bits-sorted", 0);
    REQUIRE(decline.seed_lookup_dispatches == 1u);
    REQUIRE(decline.open_addressed_lookup_dispatches == 1u);
    REQUIRE(decline.sorted_dense_lookup_dispatches == 0u);
    REQUIRE(decline.sort_proof.bitonic_sort_dispatches == 1u);
    REQUIRE(decline.sort_proof.radix_sort_operations == 0u);
    REQUIRE(decline.sort_proof.host_sort_fallbacks == 0u);
    REQUIRE(decline.planned_vulkan_allocation_bytes != 0u);
    REQUIRE(decline.actual_vulkan_allocation_bytes
            == decline.planned_vulkan_allocation_bytes);
    REQUIRE(decline.canonical_anchor_count == gpu_slab.size());
    REQUIRE(decline.canonical_anchor_order_checked);
    REQUIRE(decline.canonical_anchor_order_accepted);

    const auto cpu_t = anchor_tuples(cpu_slab);
    const auto gpu_t = anchor_tuples(gpu_slab);
    ASSERT(cpu_t == gpu_t,
           "GPU anchors differ from CPU in at least one explicit Anchor field",
           cpu_t.size());

    auto fused_slab = ch::make_arena_slab<Anchor>();
    vk::VulkanSeedLookupDecline fused_decline{};
    REQUIRE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt, idx, resident, /*vulkan_enabled=*/true,
        query_len, max_occ, kmer_size, fused_slab,
        /*fused_frontend=*/true, &fused_decline));
    REQUIRE(anchor_tuples(fused_slab) == cpu_t);
    REQUIRE(fused_decline.canonical_anchor_count == fused_slab.size());
    REQUIRE(fused_decline.canonical_anchor_order_checked);
    REQUIRE(fused_decline.canonical_anchor_order_accepted);

    cache.clear();
}

TEST_CASE("vulkan seeding: non-fused shader consumes sorted-dense resident with CPU parity",
          "[vulkan][seeding][phase-b][sorted-dense][proof]") {
    auto& runtime = vk::Runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    const auto idx = build_target();
    const auto query = build_query();
    constexpr bioseq::storage::sequence_size_t query_len = 200u;
    constexpr std::int32_t max_occ = 500;
    constexpr std::int32_t kmer_size = 15;

    context::context ctx;
    context::installed_context_scope install{ctx};
    auto cpu_slab = ch::make_arena_slab<Anchor>();
    cpu_slab.reserve(64u);
    ::bioseq::simd::collect_seed_hits<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        idx, query_len, max_occ, kmer_size, cpu_slab);

    const auto plan = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::GpuIndexRepresentation::sorted_dense);
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto resident = std::make_shared<idx_ns::GpuResidentMinimizerIndex>();
    resident->slots = runtime.create_device_buffer(
        plan.slot_buffer_bytes, usage);
    resident->positions = runtime.create_device_buffer(
        plan.position_buffer_bytes, usage);
    REQUIRE(resident->slots.mapped != nullptr);
    REQUIRE(resident->positions.mapped != nullptr);
    const auto fill = idx_ns::direct_fill_gpu_minimizer_index_sorted_dense(
        idx,
        std::span<idx_ns::GpuMinimizerSlot>{
            static_cast<idx_ns::GpuMinimizerSlot*>(resident->slots.mapped),
            static_cast<std::size_t>(plan.slot_capacity)},
        std::span<std::uint64_t>{
            static_cast<std::uint64_t*>(resident->positions.mapped),
            idx.positions.size()});
    resident->target_len = idx.target_len;
    resident->slot_mask = 0u;
    resident->bucket_count = static_cast<std::uint32_t>(fill.slot_capacity);
    resident->position_count = static_cast<std::uint32_t>(idx.positions.size());
    resident->unique_key_count = fill.unique_key_count;
    resident->slot_capacity = fill.slot_capacity;
    resident->load_ppm = fill.load_ppm;
    resident->max_probe = fill.max_probe;
    resident->representation = idx_ns::GpuIndexRepresentation::sorted_dense;

    auto gpu_slab = ch::make_arena_slab<Anchor>();
    gpu_slab.reserve(64u);
    vk::VulkanSeedLookupDecline decline{};
    REQUIRE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt, idx, resident, /*vulkan_enabled=*/true,
        query_len, max_occ, kmer_size, gpu_slab,
        /*fused_frontend=*/false, &decline));

    REQUIRE(decline.seed_lookup_dispatches == 1u);
    REQUIRE(decline.open_addressed_lookup_dispatches == 0u);
    REQUIRE(decline.sorted_dense_lookup_dispatches == 1u);
    REQUIRE(nondecreasing_ref_bits(gpu_slab));
    REQUIRE(anchor_tuples(gpu_slab) == anchor_tuples(cpu_slab));
}

TEST_CASE("vulkan seeding: capacity+1 radix is stable on real GPU for odd/even pass parity",
          "[vulkan][seeding][phase-b][radix][stability]") {
    auto& runtime = vk::Runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    const std::uint32_t bitonic_capacity =
        runtime.gpu_anchor_sort_capacity();
    REQUIRE(bitonic_capacity != 0u);
    const std::uint32_t anchor_count = bitonic_capacity + 1u;

    context::context ctx;
    context::installed_context_scope install{ctx};

    const auto run_case = [&](std::uint64_t target_position,
                              std::uint64_t target_length,
                              std::uint32_t expected_passes) {
        constexpr std::uint64_t hash = 0x7a11u;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {hash, enc(target_position, 0u)}};
        MinimizerHashmapFragmentIndex idx;
        idx.build_from_pairs(
            std::span<std::pair<std::uint64_t, std::uint64_t>>{
                pairs.data(), pairs.size()},
            target_length);

        std::vector<MinimapSketchData> query;
        query.reserve(anchor_count);
        for (std::uint32_t ordinal = 0; ordinal < anchor_count; ++ordinal) {
            query.push_back(MinimapSketchData{hash, ordinal, 0u});
        }

        idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{64} << 20};
        const auto key = idx_ns::make_gpu_index_cache_key(
            idx, idx_ns::GpuIndexCacheIdentity{});
        auto resident = cache.get_or_upload(runtime, idx, key, true);
        REQUIRE(resident != nullptr);

        auto slab = ch::make_arena_slab<Anchor>();
        vk::VulkanSeedLookupDecline decline{};
        REQUIRE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
            std::span<const MinimapSketchData>{query.data(), query.size()},
            std::nullopt, idx, resident, true,
            /*query_len=*/4096u, /*max_occ=*/500, /*kmer_size=*/15,
            slab, /*fused_frontend=*/false, &decline));

        REQUIRE(slab.size() == anchor_count);
        REQUIRE(nondecreasing_ref_bits(slab));
        REQUIRE(decline.seed_lookup_dispatches == 1u);
        REQUIRE(decline.sort_plan.backend == vk::SeedAnchorSortBackend::radix);
        REQUIRE(decline.sort_proof.bitonic_sort_dispatches == 0u);
        REQUIRE(decline.sort_proof.radix_sort_operations == 1u);
        REQUIRE(decline.sort_proof.radix_anchor_count == anchor_count);
        REQUIRE(decline.sort_proof.radix_passes == expected_passes);
        REQUIRE(decline.sort_proof.radix_histogram_dispatches
                == expected_passes);
        REQUIRE(decline.sort_proof.radix_scan_dispatches == expected_passes);
        REQUIRE(decline.sort_proof.radix_scatter_dispatches
                == expected_passes);
        REQUIRE(decline.sort_proof.host_sort_fallbacks == 0u);
        REQUIRE(decline.canonical_anchor_count == anchor_count);
        REQUIRE(decline.canonical_anchor_order_checked);
        REQUIRE(decline.canonical_anchor_order_accepted);
        REQUIRE(decline.canonical_tie_normalization_runs == 1u);
        REQUIRE(decline.canonical_tie_normalization_anchors == anchor_count);

        const std::uint64_t equal_key = slab.data()[0].ref_bits();
        for (std::uint32_t ordinal = 0; ordinal < anchor_count; ++ordinal) {
            CAPTURE(ordinal, target_position, target_length, expected_passes);
            REQUIRE(slab.data()[ordinal].ref_bits() == equal_key);
            REQUIRE(slab.data()[ordinal].ref_position == target_position);
            REQUIRE(slab.data()[ordinal].query_position == ordinal);
        }
        cache.clear();
    };

    // target_len=2^24 -> 24 local ref bits + strand bit -> four radix passes.
    run_case(/*target_position=*/123u,
             /*target_length=*/std::uint64_t{1} << 24u,
             /*expected_passes=*/4u);
    // >2^32 reference plus strand bit requires five passes and leaves the
    // sorted value array in scratch, exercising both ping-pong parities.
    run_case(/*target_position=*/(std::uint64_t{1} << 32u) + 5u,
             /*target_length=*/std::uint64_t{1} << 33u,
             /*expected_passes=*/5u);
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
    vk::VulkanSeedLookupDecline decline{};
    const bool ok = vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{query.data(), query.size()},
        std::nullopt, idx, resident, true, 200, 500, 15, gpu_slab,
        /*fused_frontend=*/false, &decline);
    ASSERT(ok, "empty-hit seed lookup should still succeed", 0);
    ASSERT(gpu_slab.size() == 0, "no-hit query produced anchors", gpu_slab.size());
    REQUIRE(decline.seed_lookup_dispatches == 0u);
    REQUIRE(decline.sort_proof == vk::SeedLookupSortExecutionProof{});
    REQUIRE(decline.canonical_anchor_count == 0u);
    REQUIRE(decline.canonical_anchor_digest_lo == vk::kSeedAnchorDigestSeedLo);
    REQUIRE(decline.canonical_anchor_digest_hi == vk::kSeedAnchorDigestSeedHi);
    REQUIRE(decline.canonical_anchor_order_checked);
    REQUIRE(decline.canonical_anchor_order_accepted);
    cache.clear();
}

TEST_CASE("vulkan seeding: empty query succeeds without fabricated dispatch proof",
          "[vulkan][seeding][phase-b][proof][empty]") {
    context::context ctx;
    context::installed_context_scope install{ctx};
    MinimizerHashmapFragmentIndex idx;
    std::vector<MinimapSketchData> empty;
    auto slab = ch::make_arena_slab<Anchor>();
    vk::VulkanSeedLookupDecline decline{};

    REQUIRE(vk::collect_seed_hits_vulkan<MinimapSketchData>(
        std::span<const MinimapSketchData>{empty.data(), empty.size()},
        std::nullopt, idx, nullptr, /*vulkan_enabled=*/true,
        /*query_len=*/200u, /*max_occ=*/500, /*kmer_size=*/15,
        slab, /*fused_frontend=*/false, &decline));
    REQUIRE(slab.size() == 0u);
    REQUIRE(decline.seed_lookup_dispatches == 0u);
    REQUIRE(decline.sort_proof == vk::SeedLookupSortExecutionProof{});
    REQUIRE(decline.canonical_anchor_count == 0u);
    REQUIRE(decline.canonical_anchor_digest_lo == vk::kSeedAnchorDigestSeedLo);
    REQUIRE(decline.canonical_anchor_digest_hi == vk::kSeedAnchorDigestSeedHi);
    REQUIRE(decline.canonical_anchor_order_checked);
    REQUIRE(decline.canonical_anchor_order_accepted);
}

TEST_CASE("vulkan seeding: typed decline maps pre-dispatch shape predicates",
          "[vulkan][seeding][phase-b][decline]") {
    const auto query = build_query();
    const auto idx = build_target();

    // ArenaSlab is bound to context::current(). Keep the context installation
    // alive for the whole slab lifetime, matching the adjacent Vulkan seeding
    // tests; reverse destruction tears down the slab before the scope/context.
    context::context ctx;
    context::installed_context_scope install{ctx};
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
            vk::VulkanSeedLookupDeclineReason::
                radix_workgroup_geometry_unsupported,
            vk::VulkanSeedLookupDeclineReason::target_coordinate_invalid,
            vk::VulkanSeedLookupDeclineReason::
                sort_storage_buffer_range_exceeded,
            vk::VulkanSeedLookupDeclineReason::
                sort_plan_arithmetic_overflow,
            vk::VulkanSeedLookupDeclineReason::negative_max_occ_unsupported,
            vk::VulkanSeedLookupDeclineReason::
                seed_transient_memory_admission_denied}) {
        REQUIRE(vk::vulkan_seed_lookup_decline_reason_name(reason) != "unknown");
    }
}

}  // namespace
