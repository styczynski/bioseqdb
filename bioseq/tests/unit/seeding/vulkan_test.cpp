/**
 * @file vulkan_test.cpp
 * @brief Compare Vulkan and CPU seed hits, including order and large target positions.
 *
 * Also check mapped-index validation, GPU sort planning, and reasons for
 * declining a lookup. GPU cases skip when hardware Vulkan is unavailable.
 */
#include <vulkan/vulkan_core.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <libassert/assert.hpp>
#include <libassert/assert-catch2.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.context;
import bioseq.sequences.kmers;
import bioseq.pipeline.minimap_port;         // collect_seed_hits
import bioseq.chaining.core;                 // anchor / arena_slab / make_arena_slab
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.seeding.vulkan;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

namespace {

using minimap_sketch_data = bioseq::indexing::minimap_sketch_data;
using bioseq::indexing::minimizer_hashmap_fragment_index;
using anchor = bioseq::chaining::anchor;
namespace ch = bioseq::chaining;
namespace seed = bioseq::seeding;
namespace vk = bioseq::gpu::vulkan;
namespace idx_ns = bioseq::indexing;

/**
 * @brief Pack a target position with its strand in the low bit.
 * @param[in] position Target position in bases.
 * @param[in] strand Zero for forward and one for reverse.
 * @return The index's encoded position.
 */
[[nodiscard]] static std::uint64_t encode_position_strand(std::uint64_t position,
                                                          std::uint64_t strand) {
    return (position << 1) | (strand & 1u);
}

/** Build an index with repeated hashes on both strands and a position above 2^32. */
[[nodiscard]] static minimizer_hashmap_fragment_index build_target() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.emplace_back(0x111ULL, encode_position_strand(5u, 0u));
    pairs.emplace_back(0x222ULL, encode_position_strand(9u, 1u));
    pairs.emplace_back(0x222ULL, encode_position_strand(40u, 0u));
    pairs.emplace_back(0x222ULL, encode_position_strand(100u, 1u));
    pairs.emplace_back(0x333ULL, encode_position_strand(13u, 0u));
    pairs.emplace_back(0x444ULL, encode_position_strand(std::uint64_t{0x1'0000'0005ULL}, 1u));
    minimizer_hashmap_fragment_index target_index;
    target_index.build_from_pairs(
        std::span<std::pair<std::uint64_t, std::uint64_t>>{pairs.data(), pairs.size()},
        std::uint64_t{0x2'0000'0000ULL});
    return target_index;
}

/** Make query minimizers covering repeated hits, both strands, and a missing hash. */
[[nodiscard]] static std::vector<minimap_sketch_data> build_query() {
    return {
        minimap_sketch_data{0x222ULL, 17u, 0u},
        minimap_sketch_data{0x444ULL, 14u, 1u},
        minimap_sketch_data{0x111ULL, 20u, 0u},
        minimap_sketch_data{0x999ULL, 30u, 0u},  // no target hit
    };
}

/**
 * @brief Copy anchor fields into tuples for CPU/GPU comparison.
 * @return Tuples in the slab's order.
 */
[[nodiscard]] static std::vector<std::tuple<std::uint64_t, std::uint64_t,
                                            std::uint32_t, std::uint8_t,
                                            std::uint8_t, std::uint16_t>>
anchor_tuples(const ch::arena_slab<anchor>& slab) {
    std::vector<std::tuple<std::uint64_t,
                           std::uint64_t,
                           std::uint32_t,
                           std::uint8_t,
                           std::uint8_t,
                           std::uint16_t>>
        tuples;
    for (std::size_t index = 0; index < slab.size(); ++index) {
        const anchor& anchor_record = slab.data()[index];
        tuples.emplace_back(anchor_record.ref_position,
                            anchor_record.query_position,
                            anchor_record.is_reverse,
                            anchor_record.query_span,
                            anchor_record.flags,
                            anchor_record.segment_id);
    }
    return tuples;
}

/** Return whether anchor reference sort keys are in nondecreasing order. */
[[nodiscard]] static bool nondecreasing_reference_sort_keys(
        const ch::arena_slab<anchor>& slab) {
    for (std::size_t index = 1; index < slab.size(); ++index) {
        if (slab.data()[index].reference_sort_key() < slab.data()[index - 1].reference_sort_key()) {
            return false;
        }
    }
    return true;
}

/** Build readback bytes and record whether the anchors follow the required order. */
[[nodiscard]] static vk::seed_lookup_result make_seed_anchor_readback(
    std::span<const vk::seed_anchor_wire> wires) {
    vk::seed_lookup_result readback{};
    readback.count = wires.size();
    readback.host_bytes.resize(wires.size_bytes());
    auto payload = std::span<std::byte>{
        readback.host_bytes.data(), readback.host_bytes.size()};
    for (std::size_t index = 0u; index < wires.size(); ++index) {
        vk::store_seed_anchor_wire(payload, index, wires[index]);
    }
    readback.anchor_order_audit = vk::audit_seed_anchor_canonical_order(
        std::span<const std::byte>{payload.data(), payload.size()},
        readback.count);
    return readback;
}

static_assert(sizeof(vk::seed_anchor_wire) == 24u);
static_assert(alignof(vk::seed_anchor_wire) == 8u);

TEST_CASE("vulkan seeding: scalable sort plan boundary and XXL arithmetic",
          "[component:bioseq][layer:unit][feature:seeding]") {
    constexpr std::uint64_t max_range =
        (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(vk::compute_seed_anchor_bitonic_capacity(
        /*max_invocations=*/1024u, /*max_size_x=*/1024u,
        /*shared_bytes=*/32u * 1024u) == 1024u);
    REQUIRE(vk::compute_seed_anchor_bitonic_capacity(
        /*max_invocations=*/1024u, /*max_size_x=*/512u,
        /*shared_bytes=*/32u * 1024u) == 512u);
    REQUIRE(vk::compute_seed_anchor_bitonic_capacity(
        /*max_invocations=*/512u, /*max_size_x=*/1024u,
        /*shared_bytes=*/32u * 1024u) == 512u);
    REQUIRE(vk::compute_seed_anchor_bitonic_capacity(
        /*max_invocations=*/1024u, /*max_size_x=*/1024u,
        /*shared_bytes=*/14'335u) == 256u);
    const auto at_capacity = vk::make_seed_anchor_sort_plan(
        1024u, 500'000'000u, 0u, 1024u, 65'535u,
        256u, 256u, max_range);
    REQUIRE(at_capacity.is_valid());
    REQUIRE(at_capacity.backend == vk::seed_anchor_sort_backend::bitonic);
    REQUIRE(at_capacity.anchor_bytes == 24'576u);
    REQUIRE(at_capacity.key_bytes == 8'192u);
    REQUIRE(at_capacity.device_sort_bytes == 32'776u);
    REQUIRE(at_capacity.host_result_bytes == 24'576u);
    REQUIRE(at_capacity.transient_bytes == 57'352u);

    const auto above_capacity = vk::make_seed_anchor_sort_plan(
        1025u, 500'000'000u, 0u, 1024u, 65'535u,
        256u, 256u, max_range);
    REQUIRE(above_capacity.is_valid());
    REQUIRE(above_capacity.backend == vk::seed_anchor_sort_backend::radix);
    REQUIRE(above_capacity.num_workgroups == 3u);
    REQUIRE(above_capacity.elements_per_workgroup == 342u);
    REQUIRE(above_capacity.histogram_bytes == 3'072u);
    REQUIRE(above_capacity.device_sort_bytes == 68'672u);
    REQUIRE(above_capacity.host_result_bytes == 24'600u);
    REQUIRE(above_capacity.transient_bytes == 93'272u);

    const auto report87 = vk::make_seed_anchor_sort_plan(
        1'520'944u, 500'000'000u, 0u, 1024u, 65'535u,
        256u, 256u, max_range);
    REQUIRE(report87.is_valid());
    REQUIRE(report87.backend == vk::seed_anchor_sort_backend::radix);
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

    const seed::vulkan_seed_buffer_allocation_requirements requirements{
        .scratch_bytes = 4u,
        .prefix_bytes = 404u,
        .anchors_bytes = report87.anchor_bytes,
        .anchor_keys_bytes = report87.key_bytes,
        .anchors_scratch_bytes = report87.anchor_bytes,
        .anchor_keys_scratch_bytes = report87.key_bytes,
        .histogram_bytes = report87.histogram_bytes,
    };
    const auto no_slab_growth = seed::make_vulkan_seed_reservation_plan(
        requirements, 404u, report87.host_result_bytes, 0u);
    REQUIRE(no_slab_growth.is_arithmetic_valid);
    REQUIRE(no_slab_growth.vulkan_allocation_requirement_bytes
            == 100'383'128u);
    REQUIRE(no_slab_growth.total_bytes == 136'886'188u);
    REQUIRE(seed::vulkan_seed_reservation_fits(
        no_slab_growth, no_slab_growth.total_bytes));
    REQUIRE_FALSE(seed::vulkan_seed_reservation_fits(
        no_slab_growth, no_slab_growth.total_bytes - 1u));

    const auto with_slab_growth = seed::make_vulkan_seed_reservation_plan(
        requirements,
        404u,
        report87.host_result_bytes,
        report87.anchor_bytes);
    REQUIRE(with_slab_growth.is_arithmetic_valid);
    REQUIRE(with_slab_growth.total_bytes == 173'388'844u);
    REQUIRE(with_slab_growth.anchors_out_growth_bytes == 36'502'656u);
}

TEST_CASE("vulkan seeding: canonical anchor audit normalizes ref ties and digests explicit fields",
          "[component:bioseq][layer:unit][feature:seeding]") {
    std::vector<vk::seed_anchor_wire> anchors{
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

    const auto audit = vk::audit_seed_anchor_canonical_order(
        std::as_bytes(std::span{anchors}), anchors.size());
    REQUIRE(audit.anchor_count == anchors.size());
    REQUIRE(audit.canonical_order_checked);
    REQUIRE(audit.canonical_order_accepted);

    auto mutated = anchors;
    mutated[1].flags = 1u;
    const auto mutated_audit = vk::audit_seed_anchor_canonical_order(
        std::as_bytes(std::span{mutated}), mutated.size());
    REQUIRE(mutated_audit.canonical_order_accepted);
    REQUIRE((mutated_audit.digest_lo != audit.digest_lo
             || mutated_audit.digest_hi != audit.digest_hi));

    auto ref_disordered = anchors;
    std::swap(ref_disordered.front(), ref_disordered.back());
    const auto rejected = vk::normalize_seed_anchor_equal_ref_ties(
        std::as_writable_bytes(std::span{ref_disordered}),
        ref_disordered.size());
    REQUIRE(rejected.gpu_ref_order_checked);
    REQUIRE_FALSE(rejected.gpu_ref_order_accepted);

    const auto empty = vk::audit_seed_anchor_canonical_order({}, 0u);
    REQUIRE(empty.anchor_count == 0u);
    REQUIRE(empty.digest_lo == vk::k_seed_anchor_digest_seed_low);
    REQUIRE(empty.digest_hi == vk::k_seed_anchor_digest_seed_high);
    REQUIRE(empty.canonical_order_checked);
    REQUIRE(empty.canonical_order_accepted);
}

TEST_CASE("vulkan seeding: MPF readback authenticates before canonical publication",
          "[component:bioseq][layer:unit][feature:seeding]") {
    constexpr std::uint32_t submitted_ordinal = 41u;
    constexpr std::uint64_t target_offset = 100u;
    constexpr std::uint64_t target_length = 20u;
    constexpr std::uint64_t query_length = 64u;
    constexpr std::uint32_t query_span = 15u;
    const std::vector<vk::seed_anchor_wire> valid_wires{
        {.ref_position = 101u, .query_position = 14u, .is_reverse = 0u,
         .query_span = 15u, .flags = 0u, .segment_id = 0u},
        {.ref_position = 105u, .query_position = 40u, .is_reverse = 1u,
         .query_span = 15u, .flags = 0u, .segment_id = 0u},
    };

    context::context ctx;
    context::installed_context_scope install{ctx};
    bioseq::simd::canonical_seed_batch batch{};
    const auto valid_readback = make_seed_anchor_readback(valid_wires);
    REQUIRE(seed::authenticate_mpf_seed_anchor_readback(
        valid_readback,
        target_offset,
        target_length,
        query_length,
        query_span,
        submitted_ordinal,
        batch));
    REQUIRE(batch.anchors.size() == valid_wires.size());
    REQUIRE(batch.jobs.size() == 1u);
    REQUIRE(batch.jobs.front().ordinal == submitted_ordinal);
    REQUIRE(batch.jobs.front().offset == 0u);
    REQUIRE(batch.jobs.front().count == valid_wires.size());
    const auto expected_anchors = std::vector{
        std::tuple{std::uint64_t{101u}, std::uint64_t{14u},
                   std::uint32_t{0u}, std::uint8_t{15u},
                   std::uint8_t{0u}, std::uint16_t{0u}},
        std::tuple{std::uint64_t{105u}, std::uint64_t{40u},
                   std::uint32_t{1u}, std::uint8_t{15u},
                   std::uint8_t{0u}, std::uint16_t{0u}},
    };
    REQUIRE(anchor_tuples(batch.anchors) == expected_anchors);
    const std::array<std::uint32_t, 1u> submitted{submitted_ordinal};
    REQUIRE(bioseq::simd::canonical_seed_jobs_match_submission(
        batch, submitted));
    batch.jobs.front().offset = 1u;
    REQUIRE_FALSE(bioseq::simd::canonical_seed_jobs_match_submission(
        batch, submitted));

    auto corrupt_digest = valid_readback;
    corrupt_digest.anchor_order_audit.digest_lo ^= 1u;
    REQUIRE_FALSE(seed::authenticate_mpf_seed_anchor_readback(
        corrupt_digest,
        target_offset,
        target_length,
        query_length,
        query_span,
        submitted_ordinal,
        batch));
    REQUIRE(batch.anchors.size() == 0u);
    REQUIRE(batch.jobs.empty());

    auto truncated = valid_readback;
    truncated.host_bytes.pop_back();
    REQUIRE_FALSE(seed::authenticate_mpf_seed_anchor_readback(
        truncated,
        target_offset,
        target_length,
        query_length,
        query_span,
        submitted_ordinal,
        batch));
    REQUIRE(batch.anchors.size() == 0u);
    REQUIRE(batch.jobs.empty());

    const auto rejects_records = [
            &batch,
            target_offset,
            target_length,
            query_length,
            query_span,
            submitted_ordinal](
            std::span<const vk::seed_anchor_wire> rejected_wires) {
        const auto rejected_readback =
            make_seed_anchor_readback(rejected_wires);
        const bool accepted = seed::authenticate_mpf_seed_anchor_readback(
            rejected_readback,
            target_offset,
            target_length,
            query_length,
            query_span,
            submitted_ordinal,
            batch);
        return !accepted
            && batch.anchors.size() == 0u
            && batch.jobs.empty();
    };

    auto invalid = valid_wires;
    invalid[0].ref_position = target_offset - 1u;
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    invalid[1].ref_position = target_offset + target_length;
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    invalid[0].query_position = query_span - 2u;
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    invalid[1].query_position = query_length;
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    invalid[0].is_reverse = 2u;
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    invalid[0].query_span = static_cast<std::uint8_t>(query_span - 1u);
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    invalid[0].flags = 1u;
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    invalid[0].segment_id = 1u;
    REQUIRE(rejects_records(invalid));
    invalid = valid_wires;
    std::swap(invalid.front(), invalid.back());
    REQUIRE(rejects_records(invalid));
}

TEST_CASE("vulkan seeding: mapped sorted-dense two-pass prefix is exact and immutable",
          "[component:bioseq][layer:unit][feature:seeding]") {
    std::vector<idx_ns::gpu_minimizer_slot> controls{
        {.hash = 0x10u, .pos_offset = 0u, .pos_count = 2u},
        {.hash = 0x20u, .pos_offset = 2u, .pos_count = 1u},
    };
    std::vector<std::uint64_t> positions{encode_position_strand(3u, 0u),
                                         encode_position_strand(5u, 1u),
                                         encode_position_strand(8u, 0u)};
    std::uint64_t checksum = idx_ns::k_gpu_source_direct_checksum_seed;
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
    const auto validation = seed::validate_mapped_sorted_dense_payload(
        controls, positions, 100u, checksum);
    REQUIRE(validation.canonical_order_checked);
    REQUIRE(validation.is_valid);
    REQUIRE(validation.covered_position_count == positions.size());

    // These hashes share their low four bits, so lookup must compare full
    // hashes. Check a missing hash and count repeated query hashes separately.
    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{0x10u, 1u, 0u},
        minimap_sketch_data{0x20u, 2u, 0u},
        minimap_sketch_data{0x30u, 3u, 0u},
        minimap_sketch_data{0x10u, 4u, 1u},
    };
    const auto query_span = std::span<const minimap_sketch_data>{query};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> oracle_pairs{
        {0x10u, positions[0]},
        {0x10u, positions[1]},
        {0x20u, positions[2]},
    };
    minimizer_hashmap_fragment_index oracle;
    oracle.build_from_pairs(oracle_pairs, 100u);

    const auto mapped_plan = seed::count_seed_prefix_plan(
        query_span,
        seed::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/500);
    const auto oracle_plan = seed::count_seed_prefix_plan(
        query_span, oracle, /*max_occ=*/500);
    REQUIRE(mapped_plan.prefix_len == query.size() + 1u);
    REQUIRE(mapped_plan.exact_anchor_count == 5u);
    REQUIRE(mapped_plan.exact_anchor_count
            == oracle_plan.exact_anchor_count);
    REQUIRE(mapped_plan.mapped_lookup_count == query.size());
    REQUIRE(mapped_plan.mapped_comparison_count >= query.size());

    const auto mapped_prefix =
        seed::build_seed_prefix_counts_after_admission(
            query_span,
            seed::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/500,
            mapped_plan);
    const auto oracle_prefix =
        seed::build_seed_prefix_counts_after_admission(
            query_span, oracle, /*max_occ=*/500, oracle_plan);
    REQUIRE((mapped_prefix.prefix
             == std::vector<std::uint32_t>{0u, 2u, 3u, 3u, 5u}));
    REQUIRE(mapped_prefix.prefix == oracle_prefix);
    REQUIRE(mapped_prefix.mapped_lookup_count == query.size());
    REQUIRE(mapped_prefix.mapped_comparison_count >= query.size());

    // Validate the mapped target in both passes even when the query has no
    // minimizers. The resulting prefix contains one zero, with no hash lookups.
    const std::span<const minimap_sketch_data> empty_query{};
    const auto empty_mapped_plan = seed::count_seed_prefix_plan(
        empty_query,
        seed::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/500);
    const auto empty_mapped_prefix =
        seed::build_seed_prefix_counts_after_admission(
            empty_query,
            seed::validated_mapped_sorted_dense_prefix_view(
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

    const auto limited_plan = seed::count_seed_prefix_plan(
        query_span,
        seed::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/1);
    const auto limited_prefix =
        seed::build_seed_prefix_counts_after_admission(
            query_span,
            seed::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/1,
            limited_plan);
    REQUIRE(limited_plan.exact_anchor_count == 1u);
    REQUIRE((limited_prefix.prefix
             == std::vector<std::uint32_t>{0u, 0u, 1u, 1u, 1u}));

    const auto disabled_plan = seed::count_seed_prefix_plan(
        query_span,
        seed::validated_mapped_sorted_dense_prefix_view(
            controls, positions, 100u, checksum),
        /*max_occ=*/-1);
    const auto disabled_prefix =
        seed::build_seed_prefix_counts_after_admission(
            query_span,
            seed::validated_mapped_sorted_dense_prefix_view(
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
        seed::build_seed_prefix_counts_after_admission(
            query_span,
            seed::validated_mapped_sorted_dense_prefix_view(
                controls, positions, 100u, checksum),
            /*max_occ=*/500,
            divergent_plan),
        seed::mapped_prefix_invalid_error);

    auto mutated_positions = positions;
    mutated_positions[1] ^= 2u;
    REQUIRE_FALSE(seed::validate_mapped_sorted_dense_payload(
        controls, mutated_positions, 100u, checksum).is_valid);

    auto mutated_controls = controls;
    mutated_controls[1].pos_offset = 1u;
    REQUIRE_FALSE(seed::validate_mapped_sorted_dense_payload(
        mutated_controls, positions, 100u, checksum).is_valid);

    // Recheck the mapped target before building the prefix. A change after
    // counting must fail before any prefix is returned.
    const auto mutated_second_pass = [
        &controls,
        &mutated_positions,
        checksum,
        query_span,
        mapped_plan] {
        auto second_view = seed::validated_mapped_sorted_dense_prefix_view(
            controls, mutated_positions, 100u, checksum);
        static_cast<void>(seed::build_seed_prefix_counts_after_admission(
            query_span, std::move(second_view), /*max_occ=*/500,
            mapped_plan));
    };
    REQUIRE_THROWS_AS(mutated_second_pass(),
                      seed::mapped_prefix_invalid_error);

    constexpr std::uint64_t u32_max =
        (std::numeric_limits<std::uint32_t>::max)();
    REQUIRE(seed::checked_seed_prefix_length(u32_max - 1u) == u32_max);
    REQUIRE(seed::checked_seed_anchor_total(u32_max - 1u, 1u) == u32_max);
    try {
        static_cast<void>(seed::checked_seed_prefix_length(u32_max));
        FAIL("u32+1 prefix length was accepted");
    } catch (const seed::seed_lookup_too_large& error) {
        REQUIRE(error.kind
                == seed::seed_lookup_overflow_kind::query_prefix_length);
        REQUIRE(error.observed == u32_max + 1u);
    }
    try {
        static_cast<void>(seed::checked_seed_anchor_total(u32_max, 1u));
        FAIL("u32+1 exact anchor total was accepted");
    } catch (const seed::seed_lookup_too_large& error) {
        REQUIRE(error.kind
                == seed::seed_lookup_overflow_kind::exact_anchor_count);
        REQUIRE(error.observed == u32_max + 1u);
    }
}

TEST_CASE("vulkan seeding: sort plan validates local-key offset order and typed failures",
          "[component:bioseq][layer:unit][feature:seeding]") {
    constexpr std::uint64_t max_range =
        (std::numeric_limits<std::uint64_t>::max)();
    const auto offset_plan = vk::make_seed_anchor_sort_plan(
        1025u, std::uint64_t{1} << 29u, std::uint64_t{1} << 32u,
        1024u, 65'535u, 256u, 256u, max_range);
    REQUIRE(offset_plan.is_valid());
    REQUIRE(offset_plan.target_ref_bits == 29u);
    REQUIRE(offset_plan.target_position_offset == (std::uint64_t{1} << 32u));
    REQUIRE(offset_plan.max_local_ref_position
            == (std::uint64_t{1} << 29u) - 1u);
    REQUIRE(offset_plan.max_absolute_ref_position
            == (std::uint64_t{1} << 32u)
                + (std::uint64_t{1} << 29u) - 1u);
    // Adding the same segment offset preserves position order, so anchors
    // can be sorted locally before conversion to target coordinates.
    constexpr std::uint64_t lhs = 17u;
    constexpr std::uint64_t rhs = 99u;
    REQUIRE(lhs < rhs);
    REQUIRE(lhs + offset_plan.target_position_offset
            < rhs + offset_plan.target_position_offset);

    const auto count_failure = vk::make_seed_anchor_sort_plan(
        std::uint64_t{1} + (std::numeric_limits<std::uint32_t>::max)(),
        1u, 0u, 1024u, 65'535u, 256u, 256u, max_range);
    REQUIRE(count_failure.failure
            == vk::seed_anchor_sort_plan_failure::anchor_count_exceeds_u32);
    const auto coordinate_failure = vk::make_seed_anchor_sort_plan(
        1025u, 1u, std::uint64_t{1} << 63u,
        1024u, 65'535u, 256u, 256u, max_range);
    REQUIRE(coordinate_failure.failure
            == vk::seed_anchor_sort_plan_failure::target_coordinate_invalid);
    const auto dispatch_failure = vk::make_seed_anchor_sort_plan(
        1025u, 1000u, 0u, 1024u, 0u, 256u, 256u, max_range);
    REQUIRE(dispatch_failure.failure
            == vk::seed_anchor_sort_plan_failure::dispatch_limit_unavailable);
    const auto geometry_failure = vk::make_seed_anchor_sort_plan(
        1025u, 1000u, 0u, 1024u, 65'535u, 128u, 128u, max_range);
    REQUIRE(geometry_failure.failure
            == vk::seed_anchor_sort_plan_failure::
                radix_workgroup_geometry_unsupported);
    const auto range_ok = vk::make_seed_anchor_sort_plan(
        1024u, 1000u, 0u, 1024u, 65'535u,
        256u, 256u, 24'576u);
    REQUIRE(range_ok.is_valid());
    const auto range_failure = vk::make_seed_anchor_sort_plan(
        1024u, 1000u, 0u, 1024u, 65'535u,
        256u, 256u, 24'575u);
    REQUIRE(range_failure.failure
            == vk::seed_anchor_sort_plan_failure::storage_buffer_range_exceeded);
    REQUIRE_FALSE(vk::is_seed_radix_result_in_scratch(4u));
    REQUIRE(vk::is_seed_radix_result_in_scratch(3u));
}

TEST_CASE("vulkan seeding: anchors match CPU collect_seed_hits (fwd/rev, >2^32)",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    auto& runtime = vk::runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    const auto idx = build_target();
    const auto query = build_query();
    const bioseq::storage::sequence_size_t query_len = 200;
    constexpr std::int32_t max_occ = 500;
    constexpr std::int32_t kmer_size = 15;

    seed::vulkan_seed_lookup_completion gpu_completion{false};
    seed::vulkan_seed_lookup_completion fused_completion{false};
    context::context ctx;
    context::installed_context_scope install{ctx};

    auto cpu_slab = ch::make_arena_slab<anchor>();
    cpu_slab.reserve(64);
    ::bioseq::simd::collect_seed_hits<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        idx, query_len, max_occ, kmer_size, cpu_slab);

    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{64} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::gpu_index_cache_identity{});
    auto resident = cache.get_or_upload(runtime, idx, key, /*cache_enabled=*/true);
    ASSERT(resident != nullptr, "resident index upload failed", 0);

    bioseq::simd::canonical_seed_batch gpu_batch{};
    seed::vulkan_seed_lookup_decline decline{};
    gpu_completion = seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt, std::addressof(idx), resident, /*vulkan_enabled=*/true,
        query_len, max_occ, kmer_size, /*submitted_ordinal=*/0u,
        gpu_batch, /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false, &decline);
    ASSERT(gpu_completion,
           "collect_seed_hits_vulkan returned false on a usable device", 0);

    ASSERT(cpu_slab.size() == gpu_batch.anchors.size(),
           fmt::format("anchor count mismatch (cpu {} vs gpu {})",
                       cpu_slab.size(), gpu_batch.anchors.size()),
           cpu_slab.size(), gpu_batch.anchors.size());
    ASSERT(cpu_slab.size() == 5,
           fmt::format("expected 5 anchors, got {}", cpu_slab.size()),
           cpu_slab.size());
    ASSERT(nondecreasing_reference_sort_keys(gpu_batch.anchors),
        "GPU anchors are not reference_sort_key-sorted", 0);
    REQUIRE(decline.seed_lookup_dispatches == 1u);
    REQUIRE(decline.open_addressed_lookup_dispatches == 1u);
    REQUIRE(decline.sorted_dense_lookup_dispatches == 0u);
    REQUIRE(decline.sort_stats.bitonic_sort_dispatches == 1u);
    REQUIRE(decline.sort_stats.radix_sort_operations == 0u);
    REQUIRE(decline.sort_stats.host_sort_fallbacks == 0u);
    REQUIRE(decline.planned_vulkan_allocation_bytes != 0u);
    REQUIRE(decline.actual_vulkan_allocation_bytes
            == decline.planned_vulkan_allocation_bytes);
    REQUIRE(decline.canonical_anchor_count == gpu_batch.anchors.size());
    REQUIRE(decline.canonical_anchor_order_checked);
    REQUIRE(decline.canonical_anchor_order_accepted);

    const auto cpu_t = anchor_tuples(cpu_slab);
    const auto gpu_t = anchor_tuples(gpu_batch.anchors);
    ASSERT(cpu_t == gpu_t,
           "GPU anchors differ from CPU in at least one explicit anchor field",
           cpu_t.size());

    bioseq::simd::canonical_seed_batch fused_batch{};
    seed::vulkan_seed_lookup_decline fused_decline{};
    fused_completion = seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt, std::addressof(idx), resident, /*vulkan_enabled=*/true,
        query_len, max_occ, kmer_size, /*submitted_ordinal=*/0u,
        fused_batch, /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/true, &fused_decline);
    REQUIRE(fused_completion);
    REQUIRE(anchor_tuples(fused_batch.anchors) == cpu_t);
    REQUIRE(fused_decline.canonical_anchor_count
            == fused_batch.anchors.size());
    REQUIRE(fused_decline.canonical_anchor_order_checked);
    REQUIRE(fused_decline.canonical_anchor_order_accepted);

    cache.clear();
}

TEST_CASE("vulkan seeding: non-fused shader consumes sorted-dense resident with CPU parity",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    auto& runtime = vk::runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    const auto idx = build_target();
    const auto query = build_query();
    constexpr bioseq::storage::sequence_size_t query_len = 200u;
    constexpr std::int32_t max_occ = 500;
    constexpr std::int32_t kmer_size = 15;

    seed::vulkan_seed_lookup_completion completion{false};
    context::context ctx;
    context::installed_context_scope install{ctx};
    auto cpu_slab = ch::make_arena_slab<anchor>();
    cpu_slab.reserve(64u);
    ::bioseq::simd::collect_seed_hits<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        idx, query_len, max_occ, kmer_size, cpu_slab);

    const auto plan = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::gpu_index_representation::source_direct_sorted_dense);
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto resident = std::make_shared<idx_ns::gpu_resident_minimizer_index>();
    resident->slots = runtime.create_device_buffer(
        plan.slot_buffer_bytes, usage);
    resident->positions = runtime.create_device_buffer(
        plan.position_buffer_bytes, usage);
    REQUIRE(resident->slots.mapped != nullptr);
    REQUIRE(resident->positions.mapped != nullptr);
    const auto fill = idx_ns::direct_fill_gpu_minimizer_index_sorted_dense(
        idx,
        std::span<idx_ns::gpu_minimizer_slot>{
            static_cast<idx_ns::gpu_minimizer_slot*>(resident->slots.mapped),
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
    resident->representation =
        idx_ns::gpu_index_representation::source_direct_sorted_dense;

    bioseq::simd::canonical_seed_batch gpu_batch{};
    seed::vulkan_seed_lookup_decline decline{};
    completion = seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt, std::addressof(idx), resident, /*vulkan_enabled=*/true,
        query_len, max_occ, kmer_size, /*submitted_ordinal=*/3u,
        gpu_batch, /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false, &decline);
    REQUIRE(completion);

    REQUIRE(decline.seed_lookup_dispatches == 1u);
    REQUIRE(decline.open_addressed_lookup_dispatches == 0u);
    REQUIRE(decline.sorted_dense_lookup_dispatches == 1u);
    REQUIRE(nondecreasing_reference_sort_keys(gpu_batch.anchors));
    REQUIRE(anchor_tuples(gpu_batch.anchors) == anchor_tuples(cpu_slab));
    REQUIRE(gpu_batch.jobs.size() == 1u);
    REQUIRE(gpu_batch.jobs.front().ordinal == 3u);
}

TEST_CASE("vulkan seeding: capacity+1 radix is stable on real GPU for odd/even pass parity",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    auto& runtime = vk::runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    const std::uint32_t bitonic_capacity =
        runtime.gpu_anchor_sort_capacity();
    REQUIRE(bitonic_capacity != 0u);
    const std::uint32_t anchor_count = bitonic_capacity + 1u;

    const auto run_case = [&runtime, anchor_count](std::uint64_t target_position,
                                                   std::uint64_t target_length,
                                                   std::uint32_t expected_passes) {
        seed::vulkan_seed_lookup_completion completion{false};
        context::context ctx;
        context::installed_context_scope install{ctx};
        constexpr std::uint64_t hash = 0x7a11u;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {hash, encode_position_strand(target_position, 0u)}};
        minimizer_hashmap_fragment_index idx;
        idx.build_from_pairs(
            std::span<std::pair<std::uint64_t, std::uint64_t>>{
                pairs.data(), pairs.size()},
            target_length);

        std::vector<minimap_sketch_data> query;
        query.reserve(anchor_count);
        for (std::uint32_t ordinal = 0; ordinal < anchor_count; ++ordinal) {
            query.push_back(minimap_sketch_data{hash, ordinal + 14u, 0u});
        }

        idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{64} << 20};
        const auto key = idx_ns::make_gpu_index_cache_key(
            idx, idx_ns::gpu_index_cache_identity{});
        auto resident = cache.get_or_upload(runtime, idx, key, true);
        REQUIRE(resident != nullptr);

        bioseq::simd::canonical_seed_batch batch{};
        seed::vulkan_seed_lookup_decline decline{};
        completion = seed::collect_seed_hits_vulkan<minimap_sketch_data>(
            std::span<const minimap_sketch_data>{query.data(), query.size()},
            std::nullopt, std::addressof(idx), resident, true,
            /*query_len=*/static_cast<std::uint64_t>(anchor_count) + 15u,
            /*max_occ=*/500, /*kmer_size=*/15,
            /*submitted_ordinal=*/9u,
            batch, /*context_backing_is_admitted=*/false,
            /*fused_frontend=*/false, &decline);
        REQUIRE(completion);

        REQUIRE(batch.anchors.size() == anchor_count);
        REQUIRE(nondecreasing_reference_sort_keys(batch.anchors));
        REQUIRE(decline.seed_lookup_dispatches == 1u);
        REQUIRE(decline.sort_plan.backend == vk::seed_anchor_sort_backend::radix);
        REQUIRE(decline.sort_stats.bitonic_sort_dispatches == 0u);
        REQUIRE(decline.sort_stats.radix_sort_operations == 1u);
        REQUIRE(decline.sort_stats.radix_anchor_count == anchor_count);
        REQUIRE(decline.sort_stats.radix_passes == expected_passes);
        REQUIRE(decline.sort_stats.radix_histogram_dispatches
                == expected_passes);
        REQUIRE(decline.sort_stats.radix_scan_dispatches == expected_passes);
        REQUIRE(decline.sort_stats.radix_scatter_dispatches
                == expected_passes);
        REQUIRE(decline.sort_stats.host_sort_fallbacks == 0u);
        REQUIRE(decline.canonical_anchor_count == anchor_count);
        REQUIRE(decline.canonical_anchor_order_checked);
        REQUIRE(decline.canonical_anchor_order_accepted);
        REQUIRE(decline.canonical_tie_normalization_runs == 1u);
        REQUIRE(decline.canonical_tie_normalization_anchors == anchor_count);

        const std::uint64_t equal_key =
            batch.anchors.data()[0].reference_sort_key();
        for (std::uint32_t ordinal = 0; ordinal < anchor_count; ++ordinal) {
            CAPTURE(ordinal, target_position, target_length, expected_passes);
            REQUIRE(batch.anchors.data()[ordinal].reference_sort_key()
                    == equal_key);
            REQUIRE(batch.anchors.data()[ordinal].ref_position
                    == target_position);
            REQUIRE(batch.anchors.data()[ordinal].query_position
                    == ordinal + 14u);
        }
        cache.clear();
    };

    // Four radix passes return the sorted values to the original buffer.
    run_case(/*target_position=*/123u,
             /*target_length=*/std::uint64_t{1} << 24u,
             /*expected_passes=*/4u);
    // The larger position needs five passes, leaving the result in the
    // scratch buffer. Check that this result is read back in the same order.
    run_case(/*target_position=*/(std::uint64_t{1} << 32u) + 5u,
             /*target_length=*/std::uint64_t{1} << 33u,
             /*expected_passes=*/5u);
}

TEST_CASE("vulkan seeding: empty result when no query minimizer hits the target",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    auto& runtime = vk::runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    const auto idx = build_target();
    std::vector<minimap_sketch_data> query{
        minimap_sketch_data{0xABCULL, 14u, 0u}};

    context::context ctx;
    context::installed_context_scope install{ctx};

    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{16} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::gpu_index_cache_identity{});
    auto resident = cache.get_or_upload(runtime, idx, key, true);

    bioseq::simd::canonical_seed_batch gpu_batch{};
    seed::vulkan_seed_lookup_decline decline{};
    const bool ok = seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt, std::addressof(idx), resident, true, 200, 500, 15,
        /*submitted_ordinal=*/11u,
        gpu_batch, /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false, &decline);
    ASSERT(ok, "empty-hit seed lookup should still succeed", 0);
    ASSERT(gpu_batch.anchors.size() == 0,
           "no-hit query produced anchors", gpu_batch.anchors.size());
    REQUIRE(gpu_batch.jobs.size() == 1u);
    REQUIRE(gpu_batch.jobs.front().ordinal == 11u);
    REQUIRE(gpu_batch.jobs.front().count == 0u);
    REQUIRE(decline.seed_lookup_dispatches == 0u);
    REQUIRE(decline.sort_stats == vk::seed_lookup_sort_execution_stats{});
    REQUIRE(decline.canonical_anchor_count == 0u);
    REQUIRE(decline.canonical_anchor_digest_lo == vk::k_seed_anchor_digest_seed_low);
    REQUIRE(decline.canonical_anchor_digest_hi == vk::k_seed_anchor_digest_seed_high);
    REQUIRE(decline.canonical_anchor_order_checked);
    REQUIRE(decline.canonical_anchor_order_accepted);
    cache.clear();
}

TEST_CASE("vulkan seeding: empty query succeeds without fabricated dispatch stats",
          "[component:bioseq][layer:unit][feature:seeding]") {
    context::context ctx;
    context::installed_context_scope install{ctx};
    minimizer_hashmap_fragment_index idx;
    std::vector<minimap_sketch_data> empty;
    bioseq::simd::canonical_seed_batch batch{};
    seed::vulkan_seed_lookup_decline decline{};

    REQUIRE(seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{empty.data(), empty.size()},
        std::nullopt, std::addressof(idx), nullptr, /*vulkan_enabled=*/true,
        /*query_len=*/200u, /*max_occ=*/500, /*kmer_size=*/15,
        /*submitted_ordinal=*/12u,
        batch, /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false, &decline));
    REQUIRE(batch.anchors.size() == 0u);
    REQUIRE(batch.jobs.size() == 1u);
    REQUIRE(batch.jobs.front().ordinal == 12u);
    REQUIRE(batch.jobs.front().count == 0u);
    REQUIRE(decline.seed_lookup_dispatches == 0u);
    REQUIRE(decline.sort_stats == vk::seed_lookup_sort_execution_stats{});
    REQUIRE(decline.canonical_anchor_count == 0u);
    REQUIRE(decline.canonical_anchor_digest_lo == vk::k_seed_anchor_digest_seed_low);
    REQUIRE(decline.canonical_anchor_digest_hi == vk::k_seed_anchor_digest_seed_high);
    REQUIRE(decline.canonical_anchor_order_checked);
    REQUIRE(decline.canonical_anchor_order_accepted);
}

TEST_CASE("vulkan seeding: typed decline maps pre-dispatch shape predicates",
          "[component:bioseq][layer:unit][feature:seeding]") {
    const auto query = build_query();
    const auto idx = build_target();

    // Keep the context installed while seed batches may allocate arena storage.
    context::context ctx;
    context::installed_context_scope install{ctx};
    bioseq::simd::canonical_seed_batch batch{};
    seed::vulkan_seed_lookup_decline decline{};

    REQUIRE_FALSE(seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt,
        std::addressof(idx),
        nullptr,
        /*vulkan_enabled=*/false,
        200,
        500,
        15,
        /*submitted_ordinal=*/0u,
        batch,
        /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == seed::vulkan_seed_lookup_decline_reason::vulkan_disabled);
    REQUIRE(decline.query_minimizer_count == query.size());

    REQUIRE_FALSE(seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt,
        std::addressof(idx),
        nullptr,
        /*vulkan_enabled=*/true,
        200,
        500,
        0,
        /*submitted_ordinal=*/0u,
        batch,
        /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == seed::vulkan_seed_lookup_decline_reason::invalid_kmer_size);

    REQUIRE_FALSE(seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt,
        std::addressof(idx),
        nullptr,
        /*vulkan_enabled=*/true,
        std::uint64_t{1} << 32,
        500,
        15,
        /*submitted_ordinal=*/0u,
        batch,
        /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == seed::vulkan_seed_lookup_decline_reason::
                query_length_exceeds_u32);

    REQUIRE_FALSE(seed::collect_seed_hits_vulkan<minimap_sketch_data>(
        std::span<const minimap_sketch_data>{query.data(), query.size()},
        std::nullopt,
        std::addressof(idx),
        nullptr,
        /*vulkan_enabled=*/true,
        200,
        500,
        15,
        /*submitted_ordinal=*/0u,
        batch,
        /*context_backing_is_admitted=*/false,
        /*fused_frontend=*/false,
        &decline));
    REQUIRE(decline.reason
            == seed::vulkan_seed_lookup_decline_reason::
                resident_index_unavailable);
}

TEST_CASE("vulkan seeding: strict typed decline preserves cache and seed admission metrics",
          "[component:bioseq][layer:unit][feature:seeding]") {
    const bioseq::parallelism::memory_admission_failure admission{
        .reason = bioseq::parallelism::memory_admission_failure_reason::
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
    const seed::vulkan_seed_lookup_decline seed_decline{
        .reason = seed::vulkan_seed_lookup_decline_reason::
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
        seed::format_vulkan_seed_lookup_decline(seed_decline);
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
        seed::throw_vulkan_seed_lookup_strict_decline(
            "Vulkan seed lookup fixture", seed_decline);
    } catch (const vk::vulkan_runtime_error& error) {
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

    const seed::vulkan_seed_lookup_decline cache_decline{
        .reason = seed::vulkan_seed_lookup_decline_reason::
            resident_index_upload_declined,
        .query_length = 5121827u,
        .query_minimizer_count = 1023214u,
        .kmer_size = 15,
        .max_occ = 500,
        .index_upload = {
            .reason = idx_ns::gpu_index_upload_decline_reason::
                upload_peak_memory_admission_denied,
            .resident_bytes = 100u,
            .upload_peak_bytes = 200u,
            .slot_bytes = 80u,
            .position_bytes = 20u,
            .admission = admission,
        },
    };
    const std::string cache_formatted =
        seed::format_vulkan_seed_lookup_decline(cache_decline);
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
          "[component:bioseq][layer:unit][feature:seeding]") {
    for (const auto reason : {
            seed::vulkan_seed_lookup_decline_reason::vulkan_disabled,
            seed::vulkan_seed_lookup_decline_reason::query_length_exceeds_u32,
            seed::vulkan_seed_lookup_decline_reason::
                query_minimizer_count_exceeds_u32,
            seed::vulkan_seed_lookup_decline_reason::
                query_prefix_length_exceeds_u32,
            seed::vulkan_seed_lookup_decline_reason::invalid_kmer_size,
            seed::vulkan_seed_lookup_decline_reason::
                hardware_device_unavailable,
            seed::vulkan_seed_lookup_decline_reason::
                index_cache_unavailable,
            seed::vulkan_seed_lookup_decline_reason::
                resident_index_upload_declined,
            seed::vulkan_seed_lookup_decline_reason::
                resident_index_unavailable,
            seed::vulkan_seed_lookup_decline_reason::
                exact_anchor_count_exceeds_u32,
            seed::vulkan_seed_lookup_decline_reason::
                dispatch_workgroup_limit_exceeded,
            seed::vulkan_seed_lookup_decline_reason::
                radix_workgroup_geometry_unsupported,
            seed::vulkan_seed_lookup_decline_reason::target_coordinate_invalid,
            seed::vulkan_seed_lookup_decline_reason::
                sort_storage_buffer_range_exceeded,
            seed::vulkan_seed_lookup_decline_reason::
                sort_plan_arithmetic_overflow,
            seed::vulkan_seed_lookup_decline_reason::
                negative_max_occ_unsupported,
            seed::vulkan_seed_lookup_decline_reason::
                seed_transient_memory_admission_denied,
            seed::vulkan_seed_lookup_decline_reason::
                mapped_prefix_authority_invalid,
            seed::vulkan_seed_lookup_decline_reason::
                seed_arena_permit_owner_unavailable}) {
        REQUIRE(seed::vulkan_seed_lookup_decline_reason_name(reason)
                != "unknown");
    }
}

}  // namespace
