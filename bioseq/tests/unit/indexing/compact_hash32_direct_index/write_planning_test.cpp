/**
 * @file write_planning_test.cpp
 * @brief Tests direct hash32 image planning and replay-safe writing.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tests/support/common.hpp>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.indexing.compact_hash32_direct_index;
import bioseq.tests.unit.indexing.compact_hash32_direct_index_support;

namespace idx = bioseq::indexing;
using namespace bioseq::tests::unit::indexing::compact_hash32_direct_index_test_support;

TEST_CASE("compact exact-domain hash32 geometry freezes canonical K15 and full-hash32 "
          "boundaries and memory economics",
          "[component:bioseq][layer:unit][feature:index]") {
    const auto zero = idx::resolve_compact_hash32_address(0u, compact_geometry(32u));
    const auto byte_boundary = idx::resolve_compact_hash32_address(8u, compact_geometry(32u));
    const auto rank_left = idx::resolve_compact_hash32_address(255u, compact_geometry(32u));
    const auto rank_right = idx::resolve_compact_hash32_address(256u, compact_geometry(32u));
    const auto maximum = idx::resolve_compact_hash32_address(
        (std::numeric_limits<std::uint32_t>::max)(), compact_geometry(32u));
    REQUIRE(zero.has_value());
    REQUIRE(byte_boundary.has_value());
    REQUIRE(rank_left.has_value());
    REQUIRE(rank_right.has_value());
    REQUIRE(maximum.has_value());
    CHECK(zero->membership_byte_ordinal == 0u);
    CHECK(zero->membership_bit == 0u);
    CHECK(byte_boundary->membership_byte_ordinal == 1u);
    CHECK(byte_boundary->membership_bit == 0u);
    CHECK(rank_left->rank_checkpoint_ordinal == 0u);
    CHECK(rank_left->bit_within_rank_group == 255u);
    CHECK(rank_right->rank_checkpoint_ordinal == 1u);
    CHECK(rank_right->bit_within_rank_group == 0u);
    CHECK(maximum->membership_byte_ordinal == idx::k_compact_hash32_full_membership_bytes - 1u);
    CHECK(maximum->membership_bit == 7u);
    CHECK(maximum->rank_checkpoint_ordinal == (std::uint64_t{1u} << 24u) - 1u);
    CHECK(maximum->bit_within_rank_group == 255u);

    compact_test_source empty;
    const auto canonical_options = idx::compact_hash32_build_options{
        .identity = identity(1'000u),
        .max_occ_cap = 500u,
    };
    const auto canonical_plan =
        idx::plan_compact_hash32_direct_index(compact_source_ref(empty), canonical_options);
    REQUIRE(canonical_plan.has_value());
    CHECK(canonical_plan->geometry.canonical_k15());
    CHECK(canonical_plan->membership_bytes == idx::k_compact_hash32_canonical_k15_membership_bytes);
    CHECK(canonical_plan->rank_bytes == idx::k_compact_hash32_canonical_k15_rank_bytes);
    CHECK(canonical_plan->image_bytes == 150'995'200u);

    auto full_options = canonical_options;
    full_options.geometry = compact_geometry(32u);
    const auto full_plan =
        idx::plan_compact_hash32_direct_index(compact_source_ref(empty), full_options);
    REQUIRE(full_plan.has_value());
    CHECK(full_plan->geometry.full_h32());
    CHECK(full_plan->membership_bytes == idx::k_compact_hash32_full_membership_bytes);
    CHECK(full_plan->rank_bytes == idx::k_compact_hash32_full_rank_bytes);
    CHECK(full_plan->image_bytes == 603'980'032u);

    constexpr std::uint64_t large_unique_estimate = 357'000'000u;
    constexpr std::uint64_t large_posting_count = 373'218'519u;
    const auto compact_estimate = idx::estimate_compact_hash32_image_bytes(
        canonical_options.geometry, large_unique_estimate, large_posting_count, 4u);
    const auto full_estimate = idx::estimate_compact_hash32_image_bytes(
        full_options.geometry, large_unique_estimate, large_posting_count, 4u);
    REQUIRE(compact_estimate.has_value());
    REQUIRE(full_estimate.has_value());
    CHECK(compact_estimate->inline_singleton_count == 340'781'481u);
    CHECK(compact_estimate->materialized_hash_count == 16'218'519u);
    CHECK(compact_estimate->materialized_posting_count == 32'437'038u);
    CHECK(compact_estimate->descriptor_bytes == 2'856'000'000u);
    CHECK(compact_estimate->posting_bytes == 129'748'152u);
    CHECK(compact_estimate->image_bytes == 3'136'743'352u);
    CHECK(full_estimate->image_bytes == 3'589'728'184u);

    constexpr std::uint64_t large_query_hash_count = 14'171'377u;
    const auto canonical_touches = idx::estimate_compact_hash32_query_touches(
        large_query_hash_count, std::uint64_t{1u} << 22u, large_query_hash_count);
    const auto full_touches = idx::estimate_compact_hash32_query_touches(
        large_query_hash_count, large_query_hash_count, large_query_hash_count);
    REQUIRE(canonical_touches.has_value());
    REQUIRE(full_touches.has_value());
    CHECK(canonical_touches->maximum_membership_bytes == 453'484'064u);
    CHECK(canonical_touches->maximum_rank_bytes == 16'777'216u);
    CHECK(canonical_touches->maximum_descriptor_bytes == 113'371'016u);
    CHECK(canonical_touches->maximum_metadata_bytes == 583'632'296u);
    CHECK(full_touches->maximum_metadata_bytes == 623'540'588u);
}

TEST_CASE("compact hash32 writer binds the persisted cap to uint16 descriptors",
          "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source empty;
    const auto maximum = compact_options(100u, (std::numeric_limits<std::uint16_t>::max)(), 9u);
    const auto accepted = idx::plan_compact_hash32_direct_index(compact_source_ref(empty), maximum);
    REQUIRE(accepted.has_value());
    CHECK(accepted->layout_revision == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(accepted->descriptor_width_bytes == idx::k_compact_hash32_direct_index_descriptor_bytes);

    auto too_wide = maximum;
    too_wide.max_occ_cap =
        static_cast<std::uint32_t>((std::numeric_limits<std::uint16_t>::max)()) + 1u;
    const auto rejected =
        idx::plan_compact_hash32_direct_index(compact_source_ref(empty), too_wide);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == idx::direct_inverted_index_error::invalid_options);
    CHECK(empty.scans == 1u);
}

TEST_CASE("compact exact-domain hash32 rejects a forged image plan before touching "
          "the destination",
          "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 7u, .global_count = 2u, .target_strand_zero_count = 2u},
                    .encoded_positions =
                        {
                            encoded_position(3u, false),
                            encoded_position(9u, false),
                        },
                },
            },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto planned =
        idx::plan_compact_hash32_direct_index(compact_source_ref(source), build_options);
    REQUIRE(planned.has_value());
    auto forged = *planned;
    forged.descriptors_offset += 64u;

    constexpr std::byte canary{0xa5};
    std::vector<std::byte> guarded(static_cast<std::size_t>(planned->image_bytes) + 32u, canary);
    const auto destination =
        std::span<std::byte>{guarded}.first(static_cast<std::size_t>(planned->image_bytes));
    const auto built = idx::build_compact_hash32_direct_index_into(
        compact_source_ref(source), build_options, forged, destination);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == idx::direct_inverted_index_error::storage_contract_mismatch);

    CHECK(source.scans == 1u);
    CHECK(std::all_of(
        guarded.begin(), guarded.end(), [canary](std::byte value) { return value == canary; }));
}

TEST_CASE("compact exact-domain hash32 bounds flapping replay sources before every "
          "planned section write",
          "[component:bioseq][layer:unit][feature:storage]") {
    const auto build_options = compact_options(100u, 3u, 9u);
    const auto require_replay_rejected = [&build_options](compact_test_source& source) {
        const auto planned =
            idx::plan_compact_hash32_direct_index(compact_source_ref(source), build_options);
        REQUIRE(planned.has_value());
        constexpr std::byte canary{0xa5};
        std::vector<std::byte> guarded(static_cast<std::size_t>(planned->image_bytes) + 32u,
                                       canary);
        const auto destination =
            std::span<std::byte>{guarded}.first(static_cast<std::size_t>(planned->image_bytes));
        const auto built = idx::build_compact_hash32_direct_index_into(
            compact_source_ref(source), build_options, *planned, destination);
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == idx::direct_inverted_index_error::replay_mismatch);
        CHECK(source.scans == 2u);
        const auto tail = std::span<const std::byte>{guarded}.subspan(
            static_cast<std::size_t>(planned->image_bytes));
        CHECK(std::all_of(
            tail.begin(), tail.end(), [canary](std::byte value) { return value == canary; }));
    };

    SECTION("replay adds an accepted run") {
        compact_test_source source{
            .runs =
                {
                    compact_test_run{
                        .header = {.hash = 20u, .global_count = 1u, .target_strand_zero_count = 1u},
                        .encoded_positions = {encoded_position(2u, false)},
                    },
                },
            .replay_runs =
                {
                    compact_test_run{
                        .header = {.hash = 20u, .global_count = 1u, .target_strand_zero_count = 1u},
                        .encoded_positions = {encoded_position(2u, false)},
                    },
                    compact_test_run{
                        .header = {.hash = 30u, .global_count = 1u, .target_strand_zero_count = 1u},
                        .encoded_positions = {encoded_position(3u, false)},
                    },
                },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay removes an accepted run") {
        compact_test_source source{
            .runs =
                {
                    compact_test_run{
                        .header = {.hash = 20u, .global_count = 1u, .target_strand_zero_count = 1u},
                        .encoded_positions = {encoded_position(2u, false)},
                    },
                },
            .replay_runs = {},
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay changes an accepted posting count") {
        compact_test_source source{
            .runs =
                {
                    compact_test_run{
                        .header = {.hash = 20u, .global_count = 1u, .target_strand_zero_count = 1u},
                        .encoded_positions = {encoded_position(2u, false)},
                    },
                },
            .replay_runs =
                {
                    compact_test_run{
                        .header = {.hash = 20u, .global_count = 2u, .target_strand_zero_count = 2u},
                        .encoded_positions =
                            {
                                encoded_position(2u, false),
                                encoded_position(4u, false),
                            },
                    },
                },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay changes a hash without changing section sizes") {
        compact_test_source source{
            .runs =
                {
                    compact_test_run{
                        .header = {.hash = 20u, .global_count = 1u, .target_strand_zero_count = 1u},
                        .encoded_positions = {encoded_position(2u, false)},
                    },
                },
            .replay_runs =
                {
                    compact_test_run{
                        .header = {.hash = 21u, .global_count = 1u, .target_strand_zero_count = 1u},
                        .encoded_positions = {encoded_position(2u, false)},
                    },
                },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }
}
