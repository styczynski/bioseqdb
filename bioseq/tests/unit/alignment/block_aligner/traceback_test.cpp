/**
 * @file traceback_test.cpp
 * @brief Checks block alignment CIGARs, reversed input and changing block sizes.
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

TEST_CASE("Block Aligner empty Global/XDrop results match scan_block.rs",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = scoring_matrix<dna4_u64>::generate(1, 1, 1);

    // The public gap settings charge opening plus extension for the firqt base.
    // q=10 and e=1 therefore match the reference's opening score of -11.
    block_aligner_parameters global = global_params();
    global.gap_open = 10;
    const reference_run both_empty = run_block("", "", nw1, global);
    CHECK(both_empty.result.score == 0);
    CHECK(both_empty.result.max_q == -1);
    CHECK(both_empty.result.max_t == -1);
    CHECK(both_empty.result.cigar.empty());
    CHECK(both_empty.stats.zero_work_calls == 1);
    CHECK(both_empty.stats.simd_dp_cells == 0);
    CHECK(both_empty.stats.is_internally_consistent());

    const reference_run empty_query = run_block("", "AAAA", nw1, global);
    CHECK(empty_query.result.score == -14);
    CHECK(empty_query.result.max_q == -1);
    CHECK(empty_query.result.max_t == 3);
    CHECK(packed_cigar_string(empty_query.result.cigar) == "4D");
    CHECK(empty_query.stats.is_internally_consistent());

    const reference_run empty_reference = run_block("AAAA", "", nw1, global);
    CHECK(empty_reference.result.score == -14);
    CHECK(empty_reference.result.max_q == 3);
    CHECK(empty_reference.result.max_t == -1);
    CHECK(packed_cigar_string(empty_reference.result.cigar) == "4I");

    const auto aa_ar = scoring_matrix<dna4_u64>::generate(4, 1, 1);
    const reference_run empty_xdrop = run_block(
        "AAAA", "", aa_ar,
        xdrop_params(block_alignment_stage::right_extension,
                     block_input_order::forward));
    CHECK(empty_xdrop.result.score == 0);
    CHECK(empty_xdrop.result.max_q == -1);
    CHECK(empty_xdrop.result.max_t == -1);
    CHECK(empty_xdrop.result.cigar.empty());
    CHECK(empty_xdrop.stats.zero_work_calls == 1);
    CHECK(empty_xdrop.stats.is_internally_consistent());
}

TEST_CASE("Block packed traceback coalesces mismatch-rich diagonals",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = scoring_matrix<dna4_u64>::generate(4, 1, 1);
    const std::string query(64, 'A');
    std::string reference;
    reference.reserve(query.size());
    for (std::size_t i = 0; i < query.size(); ++i) {
        reference += (i & 1u) == 0 ? 'A' : 'C';
    }

    block_aligner_parameters params = global_params(64, 64);
    // Expensive gaps keep this alignment on the diagonal. Matches and mismatches
    // must combine into one M run in the packed CIGAR.
    params.gap_open = 30;
    const reference_run run = run_block(query, reference, scoring, params);

    CHECK(run.result.score == 96);
    CHECK(run.result.max_q == 63);
    CHECK(run.result.max_t == 63);
    CHECK(packed_cigar_string(run.result.cigar) == "64M");
    CHECK(run.result.cigar.size() == 1);
    CHECK(run.stats.forbidden_ksw2_calls == 0);
    CHECK(run.stats.forbidden_scalar_biological_fallbacks == 0);
    CHECK(run.stats.is_internally_consistent());
}

TEST_CASE("Block Aligner native XDrop matches scan_block.rs and reports termination",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    // Scores +4 for A/A and -1 for A/C reproduce the A/A and A/R comparisons
    // in the reference X-drop fixture.
    const auto aa_ar = scoring_matrix<dna4_u64>::generate(4, 1, 1);
    const block_aligner_parameters right = xdrop_params(
        block_alignment_stage::right_extension, block_input_order::forward);

    const reference_run complete = run_block("AAAAAA", "AAACCA", aa_ar, right);
    CHECK(complete.result.score == 14);
    CHECK(complete.result.max_q == 5);
    CHECK(complete.result.max_t == 5);
    // Matches and mismatches both  become M, so their adjacent runs must, merge.
    CHECK(packed_cigar_string(complete.result.cigar) == "6M");
    CHECK_FALSE(complete.result.is_z_dropped);
    CHECK(complete.stats.native_x_drop_terminations == 0);
    CHECK(complete.stats.simd_diagonal_certificate_calls == 0);
    CHECK(complete.stats.simd_dp_cells > 0);
    CHECK(complete.stats.is_internally_consistent());

    const std::string all_a(44, 'A');
    const std::string reference = std::string(15, 'A')
        + std::string(16, 'C') + std::string(13, 'A');
    const reference_run dropped = run_block(all_a, reference, aa_ar, right);
    CHECK(dropped.result.score == 60);
    CHECK(dropped.result.max_q == 14);
    CHECK(dropped.result.max_t == 14);
    CHECK(dropped.result.is_z_dropped);
    CHECK(packed_cigar_string(dropped.result.cigar) == "15M");
    CHECK(dropped.stats.native_x_drop_calls == 1);
    CHECK(dropped.stats.native_x_drop_terminations == 1);
    CHECK(dropped.stats.right_eligible == 1);
    CHECK(dropped.stats.right_executed == 1);
    CHECK(dropped.stats.is_internally_consistent());
}

TEST_CASE("Block left extension reverses compact input without scalar fallback",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto aa_ar = scoring_matrix<dna4_u64>::generate(4, 1, 1);
    const std::string query(44, 'A');

    // Left extension reads the target backwards: 15 A bases, 16 C bases, then 13 A.
    // It should stop after the first matching run and report its CIGAR in source order.
    const std::string source_reference = std::string(13, 'A')
        + std::string(16, 'C') + std::string(15, 'A');
    const reference_run left = run_block(
        query, source_reference, aa_ar,
        xdrop_params(block_alignment_stage::left_extension,
                     block_input_order::reversed));
    CHECK(left.result.score == 60);
    CHECK(left.result.max_q == 14);
    CHECK(left.result.max_t == 14);
    CHECK(left.result.is_z_dropped);
    CHECK(packed_cigar_string(left.result.cigar) == "15M");
    CHECK(left.stats.left_eligible == 1);
    CHECK(left.stats.left_executed == 1);
    CHECK(left.stats.reversed_input_calls == 1);
    CHECK(left.stats.forward_input_calls == 0);
    CHECK(left.stats.reversed_compact_symbols == 88);
    CHECK(left.stats.forbidden_ksw2_calls == 0);
    CHECK(left.stats.forbidden_scalar_biological_fallbacks == 0);
    CHECK(left.stats.is_internally_consistent());
}

TEST_CASE("Block reversed packed traceback preserves multi-run source order",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = scoring_matrix<dna4_u64>::generate(4, 6, 1);

    // Reading backwards gives 4M4I4M4D6M. Returning the CIGAR in source order
    // reverses the runs to 6M4D4M4I4M while keeping I and D unchanged.
    const std::string source_query = "AAAAAAGGGGCCCCAAAA";
    const std::string source_reference = "AAAAAATTTTGGGGAAAA";
    block_aligner_parameters params = xdrop_params(
        block_alignment_stage::left_extension,
        block_input_order::reversed);
    params.gap_open = 1;
    params.minimum_block_size = 32;
    params.maximum_block_size = 32;
    params.x_drop_threshold = 100;

    const reference_run run = run_block(
        source_query, source_reference, scoring, params);

    CHECK(run.result.score == 46);
    CHECK(run.result.max_q == 17);
    CHECK(run.result.max_t == 17);
    CHECK_FALSE(run.result.is_z_dropped);
    CHECK(packed_cigar_string(run.result.cigar) == "6M4D4M4I4M");
    CHECK(run.stats.left_executed == 1);
    CHECK(run.stats.reversed_input_calls == 1);
    CHECK(run.stats.reversed_compact_symbols == 36);
    CHECK(run.stats.forbidden_ksw2_calls == 0);
    CHECK(run.stats.forbidden_scalar_biological_fallbacks == 0);
    CHECK(run.stats.is_internally_consistent());
}

TEST_CASE("Block padding and admitted subregion boundaries are exact",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = scoring_matrix<dna4_u64>::generate(1, 1, 1);
    const auto aa_ar = scoring_matrix<dna4_u64>::generate(4, 1, 1);
    constexpr std::array<std::size_t, 13> lengths{
        0, 1, 7, 8, 9, 15, 16, 17, 23, 31, 32, 33, 41};

    for (const std::size_t length : lengths) {
        INFO("admitted length=" << length);
        const std::string query_source = "C" + std::string(length, 'A') + "G";
        const std::string reference_source = "T" + std::string(length, 'A') + "C";
        const reference_run global = run_block_region(
            query_source, reference_source,
            1, 1 + length, 1, 1 + length,
            nw1, global_params(16, 32));
        CHECK(global.result.score == static_cast<std::int32_t>(length));
        CHECK(global.result.max_q == static_cast<std::int32_t>(length) - 1);
        CHECK(global.result.max_t == static_cast<std::int32_t>(length) - 1);
        if (length == 0) {
            CHECK(global.result.cigar.empty());
        } else {
            CHECK(packed_cigar_string(global.result.cigar)
                  == std::to_string(length) + "M");
        }
        CHECK(global.stats.is_internally_consistent());

        const reference_run reversed = run_block_region(
            query_source, reference_source,
            1, 1 + length, 1, 1 + length,
            aa_ar,
            xdrop_params(block_alignment_stage::left_extension,
                         block_input_order::reversed));
        CHECK(reversed.result.score
              == static_cast<std::int32_t>(length * 4));
        CHECK(reversed.result.max_q == static_cast<std::int32_t>(length) - 1);
        CHECK(reversed.result.max_t == static_cast<std::int32_t>(length) - 1);
        if (length == 0) {
            CHECK(reversed.result.cigar.empty());
        } else {
            CHECK(packed_cigar_string(reversed.result.cigar)
                  == std::to_string(length) + "M");
            CHECK(reversed.stats.reversed_compact_symbols == length * 2);
        }
        CHECK(reversed.stats.is_internally_consistent());
    }
}

TEST_CASE("Block adaptive path grows then shrinks with upstream semantics",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = scoring_matrix<dna4_u64>::generate(1, 1, 1);
    const std::string query = std::string(32, 'A')
        + std::string(40, 'C') + std::string(256, 'A');
    const std::string reference(288, 'A');
    const reference_run adaptive = run_block(
        query, reference, nw1, global_params(16, 64));

    // The adaptive search can miss the best alignment This fixture expects
    // two insertion runs and score 244 with block sizes 16 through 64.
    CHECK(adaptive.result.score == 244);
    CHECK(adaptive.result.max_q == 327);
    CHECK(adaptive.result.max_t == 287);
    CHECK(packed_cigar_string(adaptive.result.cigar)
          == "33M39I255M1I");
    CHECK(adaptive.stats.adaptive_grow_events > 0);
    CHECK(adaptive.stats.adaptive_shrink_events > 0);
    CHECK(adaptive.stats.is_internally_consistent());

    // A fixed 64-cell  block finds one 40-base insertion and scores 247.
    // Keep this result beside the adaptive case to catch changes in either search.
    const reference_run fixed = run_block(
        query, reference, nw1, global_params(64, 64));
    CHECK(fixed.result.score == 247);
    CHECK(fixed.result.max_q == 327);
    CHECK(fixed.result.max_t == 287);
    CHECK(packed_cigar_string(fixed.result.cigar) == "32M40I256M");
    CHECK(fixed.stats.adaptive_grow_events == 0);
    CHECK(fixed.stats.adaptive_shrink_events == 0);
    CHECK(fixed.stats.is_internally_consistent());
}

} // namespace bioseq::simd
