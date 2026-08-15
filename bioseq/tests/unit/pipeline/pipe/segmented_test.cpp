/**
 * @file segmented_test.cpp
 * @brief Exercises segmented pipeline execution.
 */
#include <tests/support/common.hpp>

#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

namespace bioseq::simd::pipe {

using namespace ::bioseq::tests::pipeline::pipe_support;

TEST_CASE("bounded ntHash segmented pipe globalizes local coordinates without fallback",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::base_alignment_algorithm;

    // Force the production geometry to its 32-Mbp floor, then place the truth
    // strictly inside the second core. This exercises two real target-range
    // sketches without introducing a test-only segmentation implementation.
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t value;
        ~restore_cap() { controller.set_index_memory_cap_bytes(value); }
    } restore_cap{admission, previous_cap};
    admission.set_index_memory_cap_bytes(192'000'000u);

    constexpr std::size_t target_length = 33'000'000;
    constexpr std::size_t truth_start = 32'250'000;
    constexpr std::size_t query_length = 40'000;
    auto target = vulkan_pipeline_test_support::make_dna4(
        target_length, 0x5E6A3E17u);
    std::vector<seq::dna4> query;
    query.reserve(query_length);
    for (auto iterator = target.begin() + truth_start + query_length;
         iterator != target.begin() + truth_start;) {
        --iterator;
        seq::dna4 complement{};
        complement.assign_rank(static_cast<std::uint8_t>(
            3u - iterator->to_rank()));
        query.push_back(complement);
    }

    vulkan_pipeline_test_support::range_t query_range(query);
    vulkan_pipeline_test_support::range_t target_range(target);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view =
        bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_target);

    minimap2_parameters params{};
    bind_fixed_highway(params);
    params.minimizer_algorithm =
        minimizer_algorithm::bounded_nthash_simd;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
    params.do_cigar = false;

    const auto result = ::bioseq::simd::
        minimap_port_segmented(query_view, target_view, params);
    REQUIRE(result.is_bounded_nthash_query_execution_consistent(true));
    REQUIRE(result.is_bounded_nthash_target_execution_consistent(true));
    REQUIRE(result.bounded_nthash_target_execution_stats.segments_combined >= 2);
    REQUIRE(result.is_simd_chaining_execution_consistent(
        params.simd_chaining_policy,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(result.exact_simd_target_execution_stats_available);
    REQUIRE_FALSE(result.partial_vulkan_stats.has_any_dispatch());

    const auto primary = std::ranges::find_if(
        result.results,
        [](const auto& match) { return match.is_primary; });
    REQUIRE(primary != result.results.end());
    REQUIRE(primary->is_reverse);
    REQUIRE(std::abs(
        static_cast<std::int64_t>(primary->target_start)
        - static_cast<std::int64_t>(truth_start)) < 15);
}

}  // namespace bioseq::simd::pipe
