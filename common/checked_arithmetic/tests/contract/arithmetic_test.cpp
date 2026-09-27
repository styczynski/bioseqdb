#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <type_traits>

import bioseq.common.checked_arithmetic;

using namespace bioseq::common;

static_assert(std::is_same_v<
    decltype(checked_add(std::uint8_t{}, std::uint8_t{})),
    std::optional<std::uint8_t>>);
static_assert(std::is_same_v<
    decltype(checked_mul(std::uint32_t{}, std::uint32_t{})),
    std::optional<std::uint32_t>>);
static_assert(std::is_same_v<
    decltype(checked_add(std::uint64_t{}, 1U)),
    std::optional<std::uint64_t>>);
static_assert(std::is_same_v<
    decltype(checked_align_up(std::uint64_t{}, std::uint64_t{})),
    std::expected<std::uint64_t, checked_arithmetic_error>>);
static_assert(checked_add(std::uint16_t{12}, std::uint16_t{30}) == 42);
static_assert(checked_mul(std::uint16_t{6}, std::uint16_t{7}) == 42);
static_assert(checked_range(8, 4, 12));

TEST_CASE("common checked add and multiply reject exact unsigned overflow",
          "[component:common][layer:contract][feature:checked_arithmetic]") {
    constexpr auto u8_max = (std::numeric_limits<std::uint8_t>::max)();
    constexpr auto u64_max = (std::numeric_limits<std::uint64_t>::max)();

    CHECK(checked_add(std::uint8_t{254}, std::uint8_t{1}) == u8_max);
    CHECK_FALSE(
        checked_add(std::uint8_t{255}, std::uint8_t{1}).has_value());
    CHECK(checked_add(u64_max - 1, std::uint64_t{1}) == u64_max);
    CHECK_FALSE(checked_add(u64_max, std::uint64_t{1}).has_value());

    CHECK(checked_mul(std::uint8_t{0}, u8_max) == 0);
    CHECK(checked_mul(std::uint8_t{15}, std::uint8_t{17}) == u8_max);
    CHECK_FALSE(
        checked_mul(std::uint8_t{16}, std::uint8_t{16}).has_value());
    CHECK_FALSE(checked_mul(u64_max, std::uint64_t{2}).has_value());
}

TEST_CASE("common checked alignment distinguishes shape from overflow",
          "[component:common][layer:contract][feature:checked_arithmetic]") {
    constexpr auto maximum =
        (std::numeric_limits<std::uint64_t>::max)();

    CHECK(checked_align_up(0, 1).value() == 0);
    CHECK(checked_align_up(9, 8).value() == 16);
    CHECK(checked_align_up(maximum - 7, 8).value() == maximum - 7);
    REQUIRE_FALSE(checked_align_up(9, 0).has_value());
    CHECK(
        checked_align_up(9, 0).error()
        == checked_arithmetic_error::invalid_alignment);
    REQUIRE_FALSE(checked_align_up(9, 3).has_value());
    CHECK(
        checked_align_up(9, 3).error()
        == checked_arithmetic_error::invalid_alignment);
    REQUIRE_FALSE(checked_align_up(maximum, 8).has_value());
    CHECK(
        checked_align_up(maximum, 8).error()
        == checked_arithmetic_error::overflow);
}

TEST_CASE("common checked range validates half-open boundaries before addition",
          "[component:common][layer:contract][feature:checked_arithmetic]") {
    constexpr auto maximum =
        (std::numeric_limits<std::uint64_t>::max)();

    CHECK(checked_range(0, 0, 0));
    CHECK(checked_range(0, maximum, maximum));
    CHECK(checked_range(maximum, 0, maximum));
    CHECK_FALSE(checked_range(8, 5, 12));
    CHECK_FALSE(checked_range(maximum, 1, maximum));
}

TEST_CASE("common saturating and scaled arithmetic retain their leaf contract",
          "[component:common][layer:contract][feature:checked_arithmetic]") {
    constexpr auto maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    CHECK(saturating_add(maximum, 1) == maximum);
    CHECK(saturating_mul(maximum, 2) == maximum);
    CHECK(checked_sub(8, 3) == 5);
    CHECK_FALSE(checked_sub(3, 8).has_value());

    constexpr expansion_ratio_limit ratio{
        .numerator = 3,
        .denominator = 2,
        .grace_bytes = 7};
    CHECK(scaled_limit(10, ratio) == 22);
    CHECK_FALSE(scaled_limit(
        10,
        expansion_ratio_limit{
            .numerator = 1,
            .denominator = 0,
            .grace_bytes = 0}).has_value());
}
