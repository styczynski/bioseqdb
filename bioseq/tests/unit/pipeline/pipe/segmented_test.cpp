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

    auto result = ::bioseq::simd::
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
    // Later completion callbacks retain secondary decisions from the merged set.
    finalize_match_result_classification(result.results);
    const auto roots = std::ranges::count_if(result.results,
        [](const auto& match) { return match.is_primary; });
    REQUIRE(roots >= 2);
    REQUIRE(std::ranges::count_if(result.results,
        [](const auto& match) { return match.is_sam_primary; }) == 1);
    // These roots cover the same complete query. They are alternative placements,
    // not disjoint pieces of a chimeric alignment.
    REQUIRE(std::ranges::none_of(result.results,
        [](const auto& match) { return match.is_supplementary; }));
    REQUIRE(std::ranges::count_if(result.results,
        [](const auto& match) { return match.is_primary && match.is_sam_secondary; }) == roots - 1);
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
    const std::array<sequence_size_t, 2> query_origins{0u,
        (sequence_size_t{1} << 32) + (sequence_size_t{1} << 31) - 500u};
    for (const sequence_size_t query_origin : query_origins) {
        CAPTURE(query_origin);
        // Drop an early weak secondary before a later root and its secondary. Both
        // parent links must be remapped before the survivor records move.
        std::array<alignment_region, 4> regions{};
        const std::array<std::int32_t, 4> scores{100, 70, 60, 55};
        for (std::size_t index = 0; index < regions.size(); ++index) {
            regions[index].query_start = query_origin + (index < 2 ? 0u : 1000u);
            regions[index].query_end = regions[index].query_start + 1000;
            regions[index].ref_start = index * 2000;
            regions[index].ref_end = regions[index].ref_start + 1000;
            regions[index].score = scores[index];
        }
        classify_primary_secondary(regions, 0.5f,
            std::numeric_limits<std::int32_t>::max(), 0);
        REQUIRE(filter_secondary_regions(regions, 0.8f, 0, 5) == 3);
        REQUIRE(regions[0].region_id == 0);
        REQUIRE(regions[0].parent == 0);
        REQUIRE(regions[1].region_id == 1);
        REQUIRE(regions[1].parent == 1);
        REQUIRE(regions[2].region_id == 2);
        REQUIRE(regions[2].parent == 1);
        std::array<match_result, 3> first_segment{};
        for (std::size_t index = 0; index < first_segment.size(); ++index)
            first_segment[index] = region_to_match_result(std::move(regions[index]), query_origin + 3000, 7000);
        REQUIRE(first_segment[1].is_primary);
        REQUIRE_FALSE(first_segment[2].is_primary);
        first_segment[1].has_dp_score = true;
        first_segment[2].is_primary = false;
        finalize_match_result_classification(first_segment, true, true);
        REQUIRE(first_segment[0].is_sam_primary);
        REQUIRE(first_segment[1].is_supplementary);

        std::array<match_result, 3> second_segment{};
        second_segment[0].query_start = query_origin + 2000;
        second_segment[0].query_end = query_origin + 3000;
        second_segment[1].query_start = query_origin + 2000;
        second_segment[1].query_end = query_origin + 3000;
        second_segment[2].query_start = query_origin + 1000;
        second_segment[2].query_end = query_origin + 2000; // Alternative to the surviving first-segment root.
        for (auto& result : second_segment) result.query_len = query_origin + 3000;
        second_segment[1].is_primary = false;
        finalize_match_result_classification(second_segment, false, false);
        REQUIRE(second_segment[0].is_sam_primary);

        std::vector<match_result> merged;
        merged.emplace_back(); // Coarse-only producer: validity must remain unavailable.
        merged.back().query_len = query_origin + 3000;
        // The first segment's original primary was outside its accepted core.
        merged.push_back(std::move(first_segment[1]));
        merged.push_back(std::move(first_segment[2]));
        merged.push_back(std::move(second_segment[0]));
        merged.push_back(std::move(second_segment[1]));
        merged.push_back(std::move(second_segment[2]));
        const std::array<std::uint64_t, 6> sam_flags{0u, 0u, 0x100u, 0x800u, 0x100u, 0x100u};
        for (unsigned pass = 0; pass != 2; ++pass) {
            const auto classified = reconcile_match_result_classification(merged, 0.5f,
                std::numeric_limits<std::int32_t>::max());
            REQUIRE(classified.has_value());
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
                REQUIRE(merged[index].query_len == query_origin + 3000);
                REQUIRE(merged[index].has_sam_classification == (index != 0));
                REQUIRE(merged[index].is_sam_secondary == (index == 2 || index == 4 || index == 5));
                REQUIRE(merged[index].alignment_ordinal == index);
                REQUIRE((merged[index].export_status() & 0x900u) == sam_flags[index]);
            }
            REQUIRE(std::ranges::count_if(merged,
                [](const auto& match) { return match.is_sam_primary; }) == 1);
            REQUIRE(std::ranges::count_if(merged,
                [](const auto& match) { return match.is_supplementary; }) == 1);
        }
        // A stronger placement in a later target segment retains producer priority.
        std::array<match_result, 2> alternatives{};
        for (std::size_t index = 0; index < alternatives.size(); ++index) {
            alternatives[index].query_start = query_origin;
            alternatives[index].query_end = query_origin + 1000;
            alternatives[index].chain_score = index == 0 ? 100 : 200;
        }
        finalize_match_result_classification(alternatives, false, false);
        REQUIRE(reconcile_match_result_classification(alternatives, 0.5f,
            std::numeric_limits<std::int32_t>::max()).has_value());
        REQUIRE(alternatives[0].is_sam_secondary);
        REQUIRE(alternatives[1].is_sam_primary);
        for (std::size_t index = 0; index < alternatives.size(); ++index) {
            REQUIRE(alternatives[index].is_primary);
            REQUIRE(alternatives[index].alignment_ordinal == index);
        }
    }
}

}  // namespace bioseq::simd::pipe
