/**
 * @file compact_direct/anchor_planner/01_contracts_test.cpp
 * @brief Packed occurrence and anchor ordering contracts.
 */

#include <hwy/highway.h>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.chaining.core;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;
import bioseq.tests.unit.indexing.minimizer_hashmap.compact_direct_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

using namespace compact_direct_test_support;

TEST_CASE("packed query occurrence layout is boundary exact",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t u32_max = (std::numeric_limits<std::uint32_t>::max)();
    constexpr std::uint64_t two_to_32 = std::uint64_t{1} << 32u;

    STATIC_REQUIRE(sizeof(packed_query_occurrence) == 8u);
    STATIC_REQUIRE(sizeof(wide_query_occurrence) == 12u);

    constexpr auto empty = plan_query_occurrence_packing(0u, 0u, 0u);
    STATIC_REQUIRE(empty.has_value());
    STATIC_REQUIRE(empty->packed());
    STATIC_REQUIRE(empty->query_bits == 0u);
    STATIC_REQUIRE(empty->ordinal_bits == 32u);
    STATIC_REQUIRE(empty->ordinal_capacity == two_to_32);
    STATIC_REQUIRE(empty->ordinal_mask == UINT32_MAX);

    constexpr auto single = plan_query_occurrence_packing(1u, u32_max, u32_max);
    STATIC_REQUIRE(single.has_value());
    STATIC_REQUIRE(single->packed());
    STATIC_REQUIRE(single->ordinal_bits == 32u);
    constexpr auto single_word = encode_packed_query_occurrence(*single, 0u, UINT32_MAX);
    STATIC_REQUIRE(single_word.has_value());
    STATIC_REQUIRE(*single_word == UINT32_MAX);
    STATIC_REQUIRE((decode_packed_query_occurrence_trusted(*single, *single_word) ==
                    query_occurrence_reference{
                        .batch_query_ordinal = 0u,
                        .query_ordinal = UINT32_MAX,
                    }));

    constexpr auto maximum_queries = plan_query_occurrence_packing(u32_max, 1u, 1u);
    STATIC_REQUIRE(maximum_queries.has_value());
    STATIC_REQUIRE(maximum_queries->packed());
    STATIC_REQUIRE(maximum_queries->query_bits == 32u);
    STATIC_REQUIRE(maximum_queries->ordinal_bits == 0u);
    STATIC_REQUIRE(maximum_queries->ordinal_capacity == 1u);
    constexpr auto maximum_query_word =
        encode_packed_query_occurrence(*maximum_queries, UINT32_MAX - 1u, 0u);
    STATIC_REQUIRE(maximum_query_word.has_value());
    STATIC_REQUIRE((decode_packed_query_occurrence_trusted(*maximum_queries, *maximum_query_word) ==
                    query_occurrence_reference{
                        .batch_query_ordinal = UINT32_MAX - 1u,
                        .query_ordinal = 0u,
                    }));

    constexpr auto maximum_occurrences = plan_query_occurrence_packing(1u, u32_max, u32_max);
    STATIC_REQUIRE(maximum_occurrences.has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(1u, two_to_32, u32_max).has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(two_to_32, 0u, 0u).has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(1u, two_to_32, two_to_32).has_value());

    constexpr auto maximum_packed = plan_query_occurrence_packing(65'536u, 65'537u, 65'536u);
    STATIC_REQUIRE(maximum_packed.has_value());
    STATIC_REQUIRE(maximum_packed->packed());
    STATIC_REQUIRE(maximum_packed->ordinal_bits == 16u);
    constexpr auto forced_wide = plan_query_occurrence_packing(65'536u, 65'538u, 65'537u);
    STATIC_REQUIRE(forced_wide.has_value());
    STATIC_REQUIRE(forced_wide->route == query_occurrence_route::wide);

    constexpr auto malformed_shift = [single] {
        auto layout = *single;
        layout.ordinal_bits = 64u;
        return layout;
    }();
    STATIC_REQUIRE_FALSE(valid_query_occurrence_packing_layout(malformed_shift));
    STATIC_REQUIRE_FALSE(encode_packed_query_occurrence(malformed_shift, 0u, 0u).has_value());
    constexpr auto malformed_mask = [single] {
        auto layout = *single;
        layout.ordinal_mask = 0u;
        return layout;
    }();
    STATIC_REQUIRE_FALSE(valid_query_occurrence_packing_layout(malformed_mask));
    STATIC_REQUIRE_FALSE(encode_packed_query_occurrence(malformed_mask, 0u, 0u).has_value());
}

TEST_CASE("canonical anchor sort preserves the 63-bit reference boundary",
          "[component:bioseq][layer:unit][feature:seeding]") {
    constexpr std::uint64_t max_reference = (std::uint64_t{1} << 63u) - 1u;
    constexpr std::uint64_t low_query_position = 7u;
    constexpr std::uint64_t high_query_position = (std::uint64_t{1} << 32u) + low_query_position;
    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto anchors = bioseq::chaining::make_arena_slab<bioseq::chaining::anchor>();
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference,
        .query_position = high_query_position,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = 0u,
        .query_position = 1u,
        .is_reverse = 1u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference,
        .query_position = low_query_position,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference - 1u,
        .query_position = 99u,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });

    sort_seed_hits(anchors);
    REQUIRE(anchors.size() == 4u);
    REQUIRE(anchors.data()[0].ref_position == max_reference - 1u);
    REQUIRE(anchors.data()[1].ref_position == max_reference);
    REQUIRE(anchors.data()[1].query_position == low_query_position);
    REQUIRE(anchors.data()[2].ref_position == max_reference);
    REQUIRE(anchors.data()[2].query_position == high_query_position);
    REQUIRE(anchors.data()[1].query_sort_key() == anchors.data()[2].query_sort_key());
    REQUIRE(anchors.data()[3].is_reverse == 1u);
    REQUIRE(std::ranges::is_sorted(
        std::span<const bioseq::chaining::anchor>{anchors.data(), anchors.size()},
        bioseq::chaining::canonical_anchor_less{}));
}

TEST_CASE("bounded u32 anchor radix selects the authenticated width exactly",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using bioseq::chaining::anchor;
    const auto make_anchor = [](std::uint64_t ref_position, std::uint32_t is_reverse) noexcept {
        return anchor{
            .ref_position = ref_position,
            .query_position = ref_position ^ 0x55u,
            .is_reverse = is_reverse,
            .query_span = 15u,
            .flags = 0u,
            .segment_id = 0u,
        };
    };
    const auto require_route =
        [](std::span<anchor> anchors, std::uint64_t target_length, bool expected_bounded_u32) {
            const auto evidence =
                bioseq::chaining::sort_canonical_anchor_primary_bounded(anchors, target_length);
            REQUIRE(evidence.bounded_u32_primary_key == expected_bounded_u32);
            REQUIRE(evidence.primary_bytes == anchors.size_bytes());
            REQUIRE_FALSE(evidence.saturated);
            REQUIRE(std::ranges::is_sorted(anchors, bioseq::chaining::canonical_anchor_less{}));
        };

    SECTION("UINT32_MAX target length uses the four-byte key") {
        constexpr std::uint64_t target_length = (std::numeric_limits<std::uint32_t>::max)();
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(17u, 0u),
            make_anchor(target_length / 2u, 0u),
        };
        require_route(anchors, target_length, true);
    }

    SECTION("UINT32_MAX plus one retains the full-width key") {
        constexpr std::uint64_t target_length =
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1u;
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(19u, 0u),
            make_anchor(target_length / 2u, 0u),
        };
        require_route(anchors, target_length, false);
    }

    SECTION("a high authenticated reference retains the full-width key") {
        constexpr std::uint64_t target_length = (std::uint64_t{1u} << 40u) + 2u;
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(23u, 0u),
            make_anchor(std::uint64_t{1u} << 39u, 0u),
        };
        require_route(anchors, target_length, false);
    }

    SECTION("a reverse-only slice preserves canonical reference order") {
        constexpr std::uint64_t target_length = 10'000u;
        std::array anchors{
            make_anchor(9'999u, 1u),
            make_anchor(3u, 1u),
            make_anchor(5'001u, 1u),
        };
        require_route(anchors, target_length, true);
        REQUIRE(std::ranges::all_of(anchors,
                                    [](const anchor& anchor) { return anchor.is_reverse == 1u; }));
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
