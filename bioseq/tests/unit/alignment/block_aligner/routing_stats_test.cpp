/**
 * @file routing_stats_test.cpp
 * @brief Checks which block alignment paths run and the counters they report
 */
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.pipeline.minimap_port;
import bioseq.chaining.core;
import bioseq.common.context;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.tests.unit.alignment.block_aligner_support;

namespace bioseq::simd {

using namespace ::bioseq::simd::block_aligner_test_support;

TEST_CASE("Block minimap projection ignores dormant KSW fields and stats are exact",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = scoring_matrix<dna4_u64>::generate(1, 1, 1);
    auto query_data = dna4_from_ascii("TTTTTTTTAAAAAAATTTTTTTTT");
    auto reference_data = dna4_from_ascii("TTAAAAAAATTTTTTTTTTTT");
    range_t query_range(query_data);
    range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_reference = bioseq::storage::make_resident_lazy_sequence(reference_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto reference_view =
        bioseq::storage::make_lazy_sequence_view(lazy_reference);

    minimap2_parameters first_params;
    first_params.gap_open1 = 1;
    first_params.gap_ext1 = 1;
    first_params.block_min_size = 16;
    first_params.block_max_size = 16;
    first_params.do_cigar = true;

    minimap2_parameters dormant_changed = first_params;
    dormant_changed.bandwidth = 7'777;
    dormant_changed.end_bonus = 9'999;
    dormant_changed.gap_open2 = 111;
    dormant_changed.gap_ext2 = 17;
    dormant_changed.zdrop = 23'456;  // Global block alignment ignores zdrop.

    block_aligner_execution_stats first_stats;
    const ksw_alignment_result first = block_aligner_align_pair<false>(
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), first_params,
        block_alignment_stage::gap_extension, &first_stats);
    block_aligner_execution_stats changed_stats;
    const ksw_alignment_result changed = block_aligner_align_pair<false>(
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), dormant_changed,
        block_alignment_stage::gap_extension, &changed_stats);

    CHECK(first.score == 7);
    CHECK(changed.score == first.score);
    CHECK(changed.max_q == first.max_q);
    CHECK(changed.max_t == first.max_t);
    CHECK(packed_cigar_string(changed.cigar)
          == packed_cigar_string(first.cigar));
    CHECK(changed_stats.configuration_identity_xor == first_stats.configuration_identity_xor);
    CHECK(changed_stats.configuration_identity_sum == first_stats.configuration_identity_sum);
    CHECK(changed_stats == first_stats);
    CHECK(first_stats.is_internally_consistent());

    const auto changed_scoring =
        scoring_matrix<dna4_u64>::generate(2, 1, 1);
    block_aligner_execution_stats scoring_stats;
    (void)block_aligner_align_pair<false>(
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), changed_scoring,
        fixed_alignment_kernels<seq::dna4>(), first_params,
        block_alignment_stage::gap_extension, &scoring_stats);
    CHECK(scoring_stats.configuration_identity_xor
          != first_stats.configuration_identity_xor);
    CHECK(scoring_stats.configuration_identity_sum
          != first_stats.configuration_identity_sum);
    CHECK(scoring_stats.is_internally_consistent());

    auto invalid_layout = nw1;
    invalid_layout.simd_scores[0] = static_cast<std::int8_t>(
        invalid_layout.simd_scores[0] + 1);
    CHECK_FALSE(invalid_layout.is_block_layout_valid());

    block_aligner_execution_stats left_stats;
    const ksw_alignment_result projected_left = block_aligner_align_pair<false>(
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), first_params,
        block_alignment_stage::left_extension, &left_stats);
    block_aligner_execution_stats right_stats;
    const ksw_alignment_result projected_right = block_aligner_align_pair<false>(
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), first_params,
        block_alignment_stage::right_extension, &right_stats);
    CHECK(projected_left.score >= 0);
    CHECK(projected_right.score >= 0);
    CHECK(left_stats.left_executed == 1);
    CHECK(left_stats.reversed_input_calls == 1);
    CHECK(left_stats.native_x_drop_calls == 1);
    CHECK(left_stats.is_internally_consistent());
    CHECK(right_stats.right_executed == 1);
    CHECK(right_stats.forward_input_calls == 1);
    CHECK(right_stats.native_x_drop_calls == 1);
    CHECK(right_stats.is_internally_consistent());

    block_aligner_execution_stats combined;
    combined.merge(first_stats);
    combined.merge(left_stats);
    combined.merge(right_stats);
    CHECK(combined.left_executed == 1);
    CHECK(combined.gap_executed == 1);
    CHECK(combined.right_executed == 1);
    CHECK(combined.global_calls == 1);
    CHECK(combined.native_x_drop_calls == 2);
    CHECK(combined.reversed_input_calls == 1);
    CHECK(combined.forward_input_calls == 2);
    CHECK(combined.forbidden_ksw2_calls == 0);
    CHECK(combined.forbidden_scalar_biological_fallbacks == 0);
    CHECK(combined.is_internally_consistent());

    block_aligner_execution_stats tampered = combined;
    ++tampered.forbidden_ksw2_calls;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    ++tampered.forbidden_scalar_biological_fallbacks;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    ++tampered.left_eligible;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    tampered.routed_region_scopes = 1;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered.left_route_candidates = tampered.left_executed;
    tampered.gap_route_candidates = tampered.gap_executed;
    tampered.right_route_candidates = tampered.right_executed;
    tampered.align_region_invocations = 1;
    tampered.serial_align_region_dispatches = 1;
    CHECK(tampered.is_internally_consistent());
    tampered = combined;
    tampered.executed_configuration_identity_xor ^= 1u;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    ++tampered.executed_configuration_identity_sum;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    ++tampered.executed_trace_calls;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    ++tampered.executed_forward_input_calls;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    ++tampered.reversed_compact_symbols;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    --tampered.nonempty_calls_with_simd;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = combined;
    tampered.is_saturated = true;
    CHECK_FALSE(tampered.is_internally_consistent());
}

TEST_CASE("Block production router executes left global-gap and right routes",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    auto query_data = dna4_from_ascii(std::string(256, 'A'));
    auto target_data = dna4_from_ascii(std::string(256, 'A'));
    range_t query_range(query_data);
    range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(lazy_target);
    const auto scoring = scoring_matrix<dna4_u64>::generate(1, 1, 1);

    std::vector<::bioseq::chaining::anchor> anchors{
        make_anchor(64), make_anchor(120)};
    std::vector<alignment_region> regions(1);
    regions[0].seed_count = 2;
    regions[0].anchor_start = 0;
    regions[0].is_reverse = false;

    minimap2_parameters params;
    params.match_score = 1;
    params.mismatch_pen = 1;
    params.gap_open1 = 1;
    params.gap_ext1 = 1;
    params.zdrop = 100;
    params.max_gap = 64;
    params.min_ksw_len = 1;
    params.block_min_size = 16;
    params.block_max_size = 32;
    params.do_cigar = true;

    block_aligner_execution_stats stats;
    align_regions<base_alignment_algorithm::block_aligner>(
        std::span<alignment_region>{regions.data(), regions.size()},
        std::span<const ::bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params, &stats);

    CHECK(regions[0].has_alignment);
    CHECK_FALSE(regions[0].cigar.empty());
    CHECK(stats.align_region_invocations == 1);
    CHECK(stats.serial_align_region_dispatches == 1);
    CHECK(stats.parallel_align_region_dispatches == 0);
    CHECK(stats.routed_region_scopes == 1);
    CHECK(stats.left_route_candidates == 1);
    CHECK(stats.gap_route_candidates == 1);
    CHECK(stats.right_route_candidates == 1);
    CHECK(stats.left_executed == 1);
    CHECK(stats.gap_executed == 1);
    CHECK(stats.right_executed == 1);
    CHECK(stats.global_calls == 1);
    CHECK(stats.native_x_drop_calls == 2);
    CHECK(stats.reversed_input_calls == 1);
    CHECK(stats.forward_input_calls == 2);
    CHECK(stats.nonempty_calls_with_simd == 3);
    CHECK(stats.forbidden_ksw2_calls == 0);
    CHECK(stats.forbidden_scalar_biological_fallbacks == 0);
    CHECK(stats.is_internally_consistent());
}

TEST_CASE("Block production stats reduction is deterministic in parallel",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    auto query_data = dna4_from_ascii(std::string(512, 'A'));
    auto target_data = dna4_from_ascii(std::string(512, 'A'));
    range_t query_range(query_data);
    range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(lazy_target);
    const auto scoring = scoring_matrix<dna4_u64>::generate(1, 1, 1);

    std::vector<::bioseq::chaining::anchor> anchors;
    std::vector<alignment_region> fixture_regions(4);
    anchors.reserve(64);
    for (std::size_t region_index = 0; region_index < 4; ++region_index) {
        fixture_regions[region_index].anchor_start =
            static_cast<std::int32_t>(anchors.size());
        fixture_regions[region_index].seed_count = 16;
        fixture_regions[region_index].is_reverse = false;
        const std::uint64_t base = 48 + region_index * 104;
        for (std::size_t seed_index = 0; seed_index < 16; ++seed_index) {
            anchors.push_back(make_anchor(base + seed_index * 4));
        }
    }

    minimap2_parameters params;
    params.match_score = 1;
    params.mismatch_pen = 1;
    params.gap_open1 = 1;
    params.gap_ext1 = 1;
    params.zdrop = 100;
    params.max_gap = 32;
    params.min_ksw_len = 1;
    params.block_min_size = 16;
    params.block_max_size = 32;
    params.do_cigar = true;

    auto run = [
                   &fixture_regions,
                   &anchors,
                   &query_view,
                   &target_view,
                   &scoring,
                   &params](bioseq::parallelism::pipeline_concurrency_mode mode) {
        auto& pool = bioseq::parallelism::global_pool::instance();
        pool.restart(bioseq::parallelism::pool_config{
            .maximum_concurrency = mode
                    == bioseq::parallelism::pipeline_concurrency_mode::single_thread
                ? 1u : 2u,
            .mode = mode,
        });
        std::vector<alignment_region> regions = fixture_regions;
        block_aligner_execution_stats stats;
        align_regions<base_alignment_algorithm::block_aligner>(
            std::span<alignment_region>{regions.data(), regions.size()},
            std::span<const ::bioseq::chaining::anchor>{
                anchors.data(), anchors.size()},
            query_view, target_view, scoring,
            fixed_alignment_kernels<seq::dna4>(), params, &stats);
        return std::pair{std::move(regions), stats};
    };

    auto [serial_regions, serial_stats] = run(
        bioseq::parallelism::pipeline_concurrency_mode::single_thread);
    auto [parallel_regions, parallel_stats] = run(
        bioseq::parallelism::pipeline_concurrency_mode::task_parallel);
    bioseq::parallelism::global_pool::instance().shutdown();

    CHECK(serial_stats.align_region_invocations == 1);
    CHECK(serial_stats.serial_align_region_dispatches == 1);
    CHECK(serial_stats.parallel_align_region_dispatches == 0);
    CHECK(parallel_stats.align_region_invocations == 1);
    CHECK(parallel_stats.serial_align_region_dispatches == 0);
    CHECK(parallel_stats.parallel_align_region_dispatches == 1);
    CHECK(serial_stats.is_internally_consistent());
    CHECK(parallel_stats.is_internally_consistent());

    serial_stats.align_region_invocations = 0;
    serial_stats.serial_align_region_dispatches = 0;
    parallel_stats.align_region_invocations = 0;
    parallel_stats.parallel_align_region_dispatches = 0;
    CHECK(serial_stats == parallel_stats);
    REQUIRE(serial_regions.size() == parallel_regions.size());
    for (std::size_t index = 0; index < serial_regions.size(); ++index) {
        CHECK(serial_regions[index].query_start
              == parallel_regions[index].query_start);
        CHECK(serial_regions[index].query_end
              == parallel_regions[index].query_end);
        CHECK(serial_regions[index].ref_start
              == parallel_regions[index].ref_start);
        CHECK(serial_regions[index].ref_end
              == parallel_regions[index].ref_end);
        CHECK(serial_regions[index].dp_score
              == parallel_regions[index].dp_score);
        CHECK(format_cigar(serial_regions[index].cigar)
              == format_cigar(parallel_regions[index].cigar));
    }
}

TEST_CASE("Block SIMD scoring covers ranks above one shuffle table",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    SKIP("aa27 is outside the scheduler's generated alignment registry");
}

TEST_CASE("Block native L16 path matches the cross-lane golden",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 1, 1);
    const auto params = global_params(16, 32);
    const std::size_t lanes =
        fixed_alignment_kernels<seq::dna4>()
            .prepare_block_global_gap_certificate(scoring, params).simd_lane_count;
    if (lanes != 16) {
        SKIP("fixed generated target exposes L=8; L=16 requires AVX2");
    }

    const std::string sequence =
        "ACGTACGTACGTACGTTGCATGCATGCATGCA"
        "AAAACCCCGGGGTTTTACGTACGTACGTACGT";
    const reference_run run = run_block_mode<false, false, dna4_u64>(
        sequence, sequence, scoring, params);
    CHECK(run.result.score == 128);
    CHECK(run.result.max_q == 63);
    CHECK(run.result.max_t == 63);
    CHECK(packed_cigar_string(run.result.cigar) == "64M");
    CHECK(run.stats.simd_eight_lane_calls == 0);
    CHECK(run.stats.simd_sixteen_lane_calls == 1);
    CHECK(run.stats.simd_dp_vector_batches > 0);
    CHECK(run.stats.is_internally_consistent());
}

TEST_CASE("Block native contract rejects unsupported geometry/mode claims",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 1, 1);
    const std::size_t lanes =
        fixed_alignment_kernels<seq::dna4>()
            .prepare_block_global_gap_certificate(scoring, global_params()).simd_lane_count;
    CHECK(is_block_aligner_lane_shape_supported(8));
    CHECK(is_block_aligner_lane_shape_supported(16));
    CHECK_FALSE(is_block_aligner_lane_shape_supported(4));
    CHECK_FALSE(is_block_aligner_lane_shape_supported(32));

    block_aligner_parameters valid = global_params();
    CHECK(validate_block_aligner_contract(valid, lanes)
          == block_aligner_contract_error::none);

    block_aligner_parameters bad_gap = valid;
    bad_gap.gap_open = 0;
    CHECK(validate_block_aligner_contract(bad_gap, lanes)
          == block_aligner_contract_error::gap_penalty);

    block_aligner_parameters bad_geometry = valid;
    bad_geometry.minimum_block_size = 24;
    bad_geometry.maximum_block_size = 32;
    CHECK(validate_block_aligner_contract(bad_geometry, lanes)
          == block_aligner_contract_error::block_geometry);

    block_aligner_parameters ksw_like_zdrop_claim = valid;
    ksw_like_zdrop_claim.x_drop_threshold = 400;
    CHECK(validate_block_aligner_contract(ksw_like_zdrop_claim, lanes)
          == block_aligner_contract_error::x_drop_threshold);

    block_aligner_parameters wrong_stage_mode = valid;
    wrong_stage_mode.stage = block_alignment_stage::left_extension;
    CHECK(validate_block_aligner_contract(wrong_stage_mode, lanes)
          == block_aligner_contract_error::stage_mode);

    block_aligner_execution_stats no_calls;
    CHECK(no_calls.is_internally_consistent());
}

} // namespace bioseq::simd
