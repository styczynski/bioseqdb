/**
 * @file segmented_test.cpp
 * @brief Check reverse-strand alignment coordinates across target segments.
 */
#include <catch2/matchers/catch_matchers.hpp>

#include <catch2/catch_test_macros.hpp>

import std;
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

    // Limit the memory budget so target segments use the 32-million-base
    // minimum. Put matching roots inside both cores to check global coordinates
    // and classification after the independently completed segments are merged.
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t value;
        ~restore_cap() { controller.set_index_memory_cap_bytes(value); }
    } restore_cap{admission, previous_cap};
    admission.set_index_memory_cap_bytes(192'000'000u);

    constexpr std::size_t target_length = 33'000'000;
    constexpr std::size_t first_truth_start = 250'000;
    constexpr std::size_t truth_start = 32'250'000;
    constexpr std::size_t query_length = 40'000;
    auto target = vulkan_pipeline_test_support::make_dna4(
        target_length, 0x5E6A3E17u);
    std::copy_n(target.begin() + truth_start, query_length,
                target.begin() + first_truth_start);
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

    for (const auto start : {first_truth_start, truth_start}) {
        const auto root = std::ranges::find_if(result.results, [start](const auto& match) {
            return match.is_primary && std::abs(static_cast<std::int64_t>(match.target_start)
                - static_cast<std::int64_t>(start)) < 15;
        });
        REQUIRE(root != result.results.end());
        REQUIRE(root->is_reverse);
    }
    const auto roots = std::ranges::count_if(result.results,
        [](const auto& match) { return match.is_primary; });
    REQUIRE(roots >= 2);
    REQUIRE(std::ranges::count_if(result.results,
        [](const auto& match) { return match.is_sam_primary; }) == 1);
    REQUIRE(std::ranges::count_if(result.results,
        [](const auto& match) { return match.is_supplementary; }) == roots - 1);
    for (std::size_t index = 0; index < result.results.size(); ++index) {
        const auto& match = result.results[index];
        REQUIRE(match.has_sam_classification);
        REQUIRE(match.alignment_ordinal == index);
        REQUIRE(match.has_mapq);
        REQUIRE_FALSE(match.has_dp_score);
        REQUIRE_FALSE(match.has_edit_distance);
    }
}

TEST_CASE("merged classification preserves producer validity after core filtering",
          "[component:bioseq][layer:unit][feature:alignment]") {
    std::array<match_result, 3> first_segment{};
    first_segment[1].has_dp_score = true;
    first_segment[2].is_primary = false;
    finalize_match_result_classification(first_segment, true, true);
    REQUIRE(first_segment[0].is_sam_primary);
    REQUIRE(first_segment[1].is_supplementary);

    std::array<match_result, 2> second_segment{};
    second_segment[1].is_primary = false;
    finalize_match_result_classification(second_segment, false, false);
    REQUIRE(second_segment[0].is_sam_primary);

    std::vector<match_result> merged;
    merged.emplace_back(); // Coarse-only producer: validity must remain unavailable.
    // The first segment's original primary was outside its accepted core.
    merged.push_back(std::move(first_segment[1]));
    merged.push_back(std::move(first_segment[2]));
    merged.push_back(std::move(second_segment[0]));
    merged.push_back(std::move(second_segment[1]));
    const std::array<std::uint64_t, 5> sam_flags{0u, 0u, 0x100u, 0x800u, 0x100u};
    for (unsigned pass = 0; pass != 2; ++pass) {
        finalize_match_result_classification(merged);
        REQUIRE_FALSE(merged[0].has_sam_classification);
        REQUIRE_FALSE(merged[0].has_mapq);
        REQUIRE_FALSE(merged[0].has_dp_score);
        REQUIRE_FALSE(merged[0].has_edit_distance);
        REQUIRE_FALSE(merged[0].is_sam_primary);
        REQUIRE_FALSE(merged[0].is_supplementary);
        REQUIRE(merged[0].export_status() == 0u);
        REQUIRE(merged[1].is_sam_primary);
        REQUIRE(merged[1].has_mapq); // A computed zero is still available.
        REQUIRE(merged[1].mapq == 0);
        REQUIRE(merged[1].has_dp_score);
        REQUIRE(merged[1].has_edit_distance);
        REQUIRE(merged[2].has_mapq);
        REQUIRE_FALSE(merged[2].has_dp_score);
        REQUIRE_FALSE(merged[2].has_edit_distance);
        REQUIRE(merged[3].is_supplementary);
        REQUIRE_FALSE(merged[3].has_mapq);
        REQUIRE_FALSE(merged[3].has_edit_distance);
        REQUIRE_FALSE(merged[4].has_mapq);
        REQUIRE_FALSE(merged[4].has_edit_distance);
        for (std::size_t index = 0; index < merged.size(); ++index) {
            REQUIRE(merged[index].has_sam_classification == (index != 0));
            REQUIRE(merged[index].alignment_ordinal == index);
            REQUIRE((merged[index].export_status() & 0x900u) == sam_flags[index]);
        }
        REQUIRE(std::ranges::count_if(merged,
            [](const auto& match) { return match.is_sam_primary; }) == 1);
        REQUIRE(std::ranges::count_if(merged,
            [](const auto& match) { return match.is_supplementary; }) == 1);
    }
}

}  // namespace bioseq::simd::pipe
