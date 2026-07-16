#include "common_tests.h"

#include "../../../bench/prefilter_pipeline_truth.hpp"

#include <array>
#include <stdexcept>

TEST_CASE(
    "prefilter pipeline truth oracle preserves empty and all-missed semantics",
    "[sneaky-snake][prefilter][truth-oracle]")
{
    using namespace bioseq::bench_prefilter_truth;

    CHECK(edit_distance_within("ACGT", "ACGT", 0u));
    CHECK_FALSE(edit_distance_within("ACGT", "ACGA", 0u));
    CHECK(edit_distance_within("ACGT", "ACGA", 1u));
    CHECK(edit_distance_within("ACGT", "ACGGT", 1u));
    CHECK_FALSE(edit_distance_within("AAAA", "TTTT", 3u));

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
