#include <tests/support/common.hpp>

#include "../../../bench/prefilter_pipeline_metrics.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

import bioseq.sequences.edit_distance_threshold;

namespace {

[[nodiscard]] std::span<const char> chars(std::string_view value) noexcept
{
    return {value.data(), value.size()};
}

} // namespace

TEST_CASE(
    "edit-distance threshold handles fixed and dynamic bands exactly",
    "[prefilter][edit-distance-threshold]")
{
    using bioseq::sequences::edit_distance_within_threshold;

    CHECK(edit_distance_within_threshold(chars("ACGT"), chars("ACGT"), 0u));
    CHECK_FALSE(edit_distance_within_threshold(
        chars("ACGT"), chars("ACGA"), 0u));
    CHECK(edit_distance_within_threshold(chars("ACGT"), chars("ACGA"), 1u));
    CHECK(edit_distance_within_threshold(chars("ACGT"), chars("ACGGT"), 1u));
    CHECK(edit_distance_within_threshold(chars("ACGGT"), chars("ACGT"), 1u));
    CHECK_FALSE(edit_distance_within_threshold(
        chars("AAAA"), chars("TTTT"), 3u));

    const std::string dynamic_query = "AAAAAAAAAAAA";
    const std::string dynamic_target = "TTTTTTTTTTTT";
    CHECK_FALSE(edit_distance_within_threshold(
        chars(dynamic_query), chars(dynamic_target), 9u));
    CHECK(edit_distance_within_threshold(
        chars(dynamic_query), chars(dynamic_target), 12u));

    CHECK(edit_distance_within_threshold(chars(""), chars(""), 0u));
    CHECK(edit_distance_within_threshold(chars(""), chars("AAAA"), 4u));
    CHECK_FALSE(edit_distance_within_threshold(chars(""), chars("AAAA"), 3u));
    CHECK(edit_distance_within_threshold(
        chars("A"), chars("A"),
        (std::numeric_limits<std::uint32_t>::max)()));

    const std::array<std::uint8_t, 5u> rank_query{0u, 1u, 2u, 3u, 4u};
    const std::array<std::uint8_t, 5u> rank_target{0u, 1u, 9u, 3u, 4u};
    CHECK(edit_distance_within_threshold(
        std::span<const std::uint8_t>{rank_query},
        std::span<const std::uint8_t>{rank_target}, 1u));
}

TEST_CASE(
    "prefilter pipeline correctness metrics preserve empty and paired semantics",
    "[prefilter][pipeline-metrics]")
{
    using namespace bioseq::bench_prefilter_metrics;

    CHECK(oriented_truth_sequence("AACG", false) == "AACG");
    CHECK(oriented_truth_sequence("AACG", true) == "CGTT");
    CHECK_THROWS_AS(oriented_truth_sequence("AACN", true),
                    std::invalid_argument);

    CHECK(primary_correct_fraction(0u, 0u) == 1.0);
    CHECK(primary_correct_fraction(0u, 1u) == 0.0);
    CHECK(primary_correct_fraction(1u, 2u) == 0.5);
    CHECK_THROWS_AS(primary_correct_fraction(2u, 1u), std::logic_error);

    const std::array<std::uint8_t, 2u> mixed_truth{1u, 0u};
    const std::array<std::uint8_t, 2u> swapped_correct{0u, 1u};
    const auto mixed = summarize_primary_correctness(
        mixed_truth, swapped_correct);
    CHECK(mixed.truth_pairs == 1u);
    CHECK(mixed.correct_truth_pairs == 0u);
    CHECK(mixed.observed_correct_pairs == 1u);
    CHECK(mixed.fraction == 0.0);
}
