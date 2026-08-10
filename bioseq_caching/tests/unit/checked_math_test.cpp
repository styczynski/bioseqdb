#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

import bioseq.cache.core;
import bioseq.common.checked_arithmetic;

static_assert(std::is_same_v<bioseq::cache::math_error, bioseq::common::checked_arithmetic_error>);
static_assert(&bioseq::cache::checked_align_up == &bioseq::common::checked_align_up);
static_assert(&bioseq::cache::checked_range == &bioseq::common::checked_range);

TEST_CASE("cache checked arithmetic is the canonical common implementation",
          "[component:cache][layer:unit][feature:result_cache]") {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::array add_cases{std::pair{std::uint64_t{0}, std::uint64_t{0}},
                                   std::pair{std::uint64_t{4}, std::uint64_t{7}},
                                   std::pair{maximum - 1, std::uint64_t{1}},
                                   std::pair{maximum, std::uint64_t{1}}};
    for (const auto [left, right] : add_cases) {
        CHECK(bioseq::cache::checked_add(left, right) == bioseq::common::checked_add(left, right));
        CHECK(bioseq::cache::checked_mul(left, right) == bioseq::common::checked_mul(left, right));
    }

    constexpr std::array alignment_cases{std::pair{std::uint64_t{0}, std::uint64_t{1}},
                                         std::pair{std::uint64_t{9}, std::uint64_t{8}},
                                         std::pair{std::uint64_t{9}, std::uint64_t{3}},
                                         std::pair{maximum, std::uint64_t{8}}};
    for (const auto [value, alignment] : alignment_cases) {
        const auto cache_result = bioseq::cache::checked_align_up(value, alignment);
        const auto common_result = bioseq::common::checked_align_up(value, alignment);
        REQUIRE(cache_result.has_value() == common_result.has_value());
        if (cache_result.has_value()) {
            CHECK(*cache_result == *common_result);
        } else {
            CHECK(cache_result.error() == common_result.error());
        }
    }

    CHECK(bioseq::cache::checked_range(8, 4, 12) == bioseq::common::checked_range(8, 4, 12));
    CHECK(bioseq::cache::checked_range(maximum, 1, maximum) ==
          bioseq::common::checked_range(maximum, 1, maximum));
}

TEST_CASE("cache wire errors retain the consolidated arithmetic vocabulary",
          "[component:cache][layer:unit][feature:result_cache]") {
    constexpr std::array<std::byte, 1> source{std::byte{0x2a}};
    std::size_t cursor = source.size();
    const auto decoded = bioseq::cache::read_little_endian<std::uint8_t>(source, cursor);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == bioseq::cache::math_error::truncated);

    const auto subspan = bioseq::cache::checked_subspan(std::span<const std::byte>{source}, 1, 1);
    REQUIRE_FALSE(subspan.has_value());
    CHECK(subspan.error() == bioseq::cache::math_error::out_of_bounds);
}
