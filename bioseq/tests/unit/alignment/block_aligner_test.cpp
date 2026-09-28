/**
 * @file block_aligner_test.cpp
 * @brief Checks block alignment scores and the shortcut for provably optimal ungapped alignments.
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

TEST_CASE("scoring_matrix boundary adapters exhaustively match the canonical facade",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    check_scoring_matrix_facade_parity<seq::dna4>("dna4");
    check_scoring_matrix_facade_parity<seq::dna5>("dna5");
    check_scoring_matrix_facade_parity<seq::aa20>("aa20");
    check_scoring_matrix_facade_parity<seq::aa27>("aa27");
}

TEST_CASE("Block Aligner Global score/endpoints/CIGAR match scan_block.rs",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = scoring_matrix<dna4_u64>::generate(1, 1, 1);

    // Reference traceback fixture with a 16-cell block and gap scores -2/-1.
    const reference_run first = run_block(
        "TTTTTTTTAAAAAAATTTTTTTTT",
        "TTAAAAAAATTTTTTTTTTTT",
        nw1, global_params());
    CHECK(first.result.score == 7);
    CHECK(first.result.max_q == 23);
    CHECK(first.result.max_t == 20);
    CHECK(packed_cigar_string(first.result.cigar) == "2M6I16M3D");
    CHECK(first.stats.is_internally_consistent());
    CHECK(first.stats.gap_eligible == 1);
    CHECK(first.stats.gap_executed == 1);
    CHECK(first.stats.global_calls == 1);
    CHECK(first.stats.forward_input_calls == 1);
    CHECK(first.stats.simd_dp_vector_batches > 0);
    CHECK(first.stats.simd_dp_cells > 0);
    CHECK(first.stats.forbidden_ksw2_calls == 0);
    CHECK(first.stats.forbidden_scalar_biological_fallbacks == 0);

    // Reference traceback fixture with a 32-cell block and gap scores -2/-1.
    const reference_run second = run_block(
        "AAAAAAAAATTGCGCT", "AAAAAAAAAGCGC",
        nw1, global_params(32, 32));
    CHECK(second.result.score == 8);
    CHECK(second.result.max_q == 15);
    CHECK(second.result.max_t == 12);
    CHECK(packed_cigar_string(second.result.cigar) == "9M2I4M1I");

    // The 20-base inserton makes the initial 16-cell block grow.
    // The expected alignment keeps all 32 matches and pays for one gap.
    const reference_run adaptive = run_block(
        std::string(16, 'A') + std::string(20, 'C') + std::string(16, 'A'),
        std::string(32, 'A'),
        nw1, global_params(16, 32));
    CHECK(adaptive.result.score == 11);
    CHECK(adaptive.result.max_q == 51);
    CHECK(adaptive.result.max_t == 31);
    CHECK(packed_cigar_string(adaptive.result.cigar) == "16M20I16M");
    CHECK(adaptive.stats.adaptive_grow_events > 0);
    CHECK(adaptive.stats.is_internally_consistent());

    const reference_run score_only = run_block(
        "TTTTTTTTAAAAAAATTTTTTTTT",
        "TTAAAAAAATTTTTTTTTTTT",
        nw1, global_params(16, 16, false));
    CHECK(score_only.result.score == first.result.score);
    CHECK(score_only.result.max_q == first.result.max_q);
    CHECK(score_only.result.max_t == first.result.max_t);
    CHECK(score_only.result.cigar.empty());
    CHECK(score_only.stats.trace_calls == 0);
    CHECK(score_only.stats.is_internally_consistent());
}

TEST_CASE("Block Global diagonal certificate is byte-identical to forced DP",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);
    block_aligner_parameters params = global_params(16, 32);
    params.gap_open = 4;
    params.gap_extend = 2;

    const auto compare_dna4 = [&scoring, &params](
            std::string_view query,
            std::string_view reference,
            bool expect_certificate) {
        const reference_run optimized =
            run_block_mode<false, true, dna4_u64>(
                query, reference, scoring, params);
        const reference_run forced_dp =
            run_block_mode<false, false, dna4_u64>(
                query, reference, scoring, params);
        check_align_result_identical(optimized.result, forced_dp.result);
        CHECK(optimized.stats.is_internally_consistent());
        CHECK(forced_dp.stats.is_internally_consistent());
        CHECK(optimized.stats.simd_diagonal_certificate_calls
              == static_cast<std::uint64_t>(expect_certificate));
        CHECK(forced_dp.stats.simd_diagonal_certificate_calls == 0);
        CHECK(forced_dp.stats.simd_dp_cells > 0);
        CHECK(forced_dp.construction.align_arena_scopes == 1);
        CHECK(forced_dp.construction.padded_sequence_allocations == 2);
        CHECK(forced_dp.construction.scratch_allocations == 1);
        CHECK(forced_dp.construction.trace_allocations == 1);
        if (expect_certificate) {
            CHECK(optimized.stats.simd_dp_cells == 0);
            CHECK(optimized.stats.simd_dp_vector_batches == 0);
            CHECK(optimized.stats.simd_diagonal_certificate_vector_batches > 0);
            CHECK(optimized.stats.simd_diagonal_certificate_symbols
                  == query.size());
            CHECK(optimized.construction.align_arena_scopes == 0);
            CHECK(optimized.construction.padded_sequence_allocations == 0);
            CHECK(optimized.construction.scratch_allocations == 0);
            CHECK(optimized.construction.trace_allocations == 0);
        } else {
            CHECK(optimized.stats.simd_dp_cells > 0);
            CHECK(optimized.stats.simd_diagonal_certificate_symbols == 0);
            CHECK(optimized.construction.align_arena_scopes == 1);
            CHECK(optimized.construction.padded_sequence_allocations == 2);
            CHECK(optimized.construction.scratch_allocations == 1);
            CHECK(optimized.construction.trace_allocations == 1);
        }
    };

    const std::string exact =
        "ACGTACGTTGCATGCAAAAACCCCGGGGTTTT"
        "TGCATGCAACGTACGTCCCCAAAATTTTGGGG";
    compare_dna4(exact, exact, true);

    // Cross the 1024-symbol scan boundary and leave one byte in the last SIMD batch.
    // Exercise reads at the  sequence end, including NEON full-vector loads.
    std::string long_unaligned_exact(1057, 'A');
    constexpr std::string_view dna4_cycle = "ACGT";
    for (std::size_t i = 0; i < long_unaligned_exact.size(); ++i) {
        long_unaligned_exact[i] = dna4_cycle[i % dna4_cycle.size()];
    }
    compare_dna4(long_unaligned_exact, long_unaligned_exact, true);

    block_aligner_parameters score_only_params = params;
    score_only_params.is_trace_enabled = false;
    const reference_run score_only_optimized =
        run_block_mode<false, true, dna4_u64>(
            exact, exact, scoring, score_only_params);
    const reference_run score_only_forced =
        run_block_mode<false, false, dna4_u64>(
            exact, exact, scoring, score_only_params);
    check_align_result_identical(
        score_only_optimized.result, score_only_forced.result);
    CHECK(score_only_optimized.result.cigar.empty());
    CHECK(score_only_optimized.stats.simd_diagonal_certificate_calls == 1);
    CHECK(score_only_optimized.stats.simd_dp_cells == 0);
    CHECK(score_only_optimized.construction.align_arena_scopes == 0);
    CHECK(score_only_optimized.construction.padded_sequence_allocations == 0);
    CHECK(score_only_optimized.construction.scratch_allocations == 0);
    CHECK(score_only_optimized.construction.trace_allocations == 0);

    block_aligner_execution_stats tampered = score_only_optimized.stats;
    tampered.simd_diagonal_certificate_vector_batches = 0;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = score_only_optimized.stats;
    tampered.simd_diagonal_certificate_symbols = 0;
    CHECK_FALSE(tampered.is_internally_consistent());
    tampered = score_only_optimized.stats;
    tampered.simd_dp_cells = 1;
    CHECK_FALSE(tampered.is_internally_consistent());

    std::string one_substitution = exact;
    one_substitution[17] = one_substitution[17] == 'A' ? 'C' : 'A';
    compare_dna4(exact, one_substitution, true);

    std::string two_substitutions = one_substitution;
    two_substitutions[46] = two_substitutions[46] == 'A' ? 'C' : 'A';
    compare_dna4(exact, two_substitutions, true);

    // Moving C by one position creates two mismatches. The score bound still
    // proves that aligning without gaps gives the unique best result.
    std::string repeat_query(64, 'A');
    repeat_query[31] = 'C';
    std::string repeat_shifted = repeat_query;
    repeat_shifted[31] = 'A';
    repeat_shifted[32] = 'C';
    compare_dna4(repeat_query, repeat_shifted, true);

    // Three substitutions fail the score bound, so alignment must use dynamic programming.
    std::string three_substitutions = two_substitutions;
    three_substitutions[55] = three_substitutions[55] == 'A' ? 'C' : 'A';
    compare_dna4(exact, three_substitutions, false);

    // A shifted sequence and unequal lengths must both use dynamic programming.
    const std::string displaced = "T" + exact.substr(0, exact.size() - 1);
    compare_dna4(exact, displaced, false);
    compare_dna4(exact, exact + "A", false);

    // A tied score bound must use dynamic programming to preserve traceback choices.
    block_aligner_parameters tie_params = params;
    tie_params.gap_open = 1;
    tie_params.gap_extend = 1;
    const reference_run tie_optimized =
        run_block_mode<false, true, dna4_u64>(
            exact, one_substitution, scoring, tie_params);
    const reference_run tie_forced =
        run_block_mode<false, false, dna4_u64>(
            exact, one_substitution, scoring, tie_params);
    check_align_result_identical(tie_optimized.result, tie_forced.result);
    CHECK(tie_optimized.stats.simd_diagonal_certificate_calls == 0);
    CHECK(tie_optimized.stats.simd_dp_cells > 0);

    // This custom matrix is valid for alignment but unsupported by the diagonal shortcut.
    auto unsupported_scoring = scoring;
    unsupported_scoring.at(0, 1) = 3;
    unsupported_scoring.simd_scores[1] = 3;
    REQUIRE(unsupported_scoring.is_block_layout_valid());
    const reference_run unsupported_optimized =
        run_block_mode<false, true, dna4_u64>(
            exact, exact, unsupported_scoring, params);
    const reference_run unsupported_forced =
        run_block_mode<false, false, dna4_u64>(
            exact, exact, unsupported_scoring, params);
    check_align_result_identical(
        unsupported_optimized.result, unsupported_forced.result);
    CHECK(unsupported_optimized.stats.simd_diagonal_certificate_calls == 0);
    CHECK(unsupported_optimized.stats.simd_dp_cells > 0);

    const auto dna5_scoring = scoring_matrix<dna5_u64>::generate(2, 4, 1);
    const std::string dna5_sequence =
        "ACGTNACGTACGTACGTACGTACGTACGTACG"
        "TGCATGCATGCATGCATGCATGCATGCATGCA";
    const reference_run dna5_optimized =
        run_block_mode<false, true, dna5_u64>(
            dna5_sequence, dna5_sequence, dna5_scoring, params);
    const reference_run dna5_forced =
        run_block_mode<false, false, dna5_u64>(
            dna5_sequence, dna5_sequence, dna5_scoring, params);
    check_align_result_identical(dna5_optimized.result, dna5_forced.result);
    CHECK(dna5_optimized.stats.simd_diagonal_certificate_calls == 1);
    CHECK(dna5_optimized.stats.simd_dp_cells == 0);
    CHECK(dna5_optimized.construction.align_arena_scopes == 0);
    CHECK(dna5_optimized.construction.padded_sequence_allocations == 0);
    CHECK(dna5_optimized.construction.scratch_allocations == 0);

    // Check the shortcut against dynamic programming on  reverse-complement query bases
    const std::string reverse_source =
        "AACCGTACGTTTGGGCCCAAATTTACGTACGT"
        "GGGGAAAACCCCTTTTACGTTGCATGCACGTA";
    const std::string reverse_target =
        reverse_complement_ascii(reverse_source);
    const reference_run reverse_optimized =
        run_block_mode<true, true, dna4_u64>(
            reverse_source, reverse_target, scoring, params);
    const reference_run reverse_forced =
        run_block_mode<true, false, dna4_u64>(
            reverse_source, reverse_target, scoring, params);
    check_align_result_identical(
        reverse_optimized.result, reverse_forced.result);
    CHECK(reverse_optimized.stats.simd_diagonal_certificate_calls == 1);
    CHECK(reverse_optimized.stats.simd_dp_cells == 0);
    CHECK(reverse_optimized.construction.align_arena_scopes == 0);
    CHECK(reverse_optimized.construction.padded_sequence_allocations == 0);
    CHECK(reverse_optimized.construction.scratch_allocations == 0);

    // Check that a reverse-complement subregion stays continuous across the
    // 1024-symbol scan boundary, even when it starts inside the, sequence.
    std::string reverse_subregion_source(1129, 'A');
    for (std::size_t i = 0; i < reverse_subregion_source.size(); ++i) {
        reverse_subregion_source[i] =
            dna4_cycle[(i * 3u + i / 11u) % dna4_cycle.size()];
    }
    const std::string reverse_subregion_full =
        reverse_complement_ascii(reverse_subregion_source);
    constexpr std::size_t reverse_query_start = 17;
    constexpr std::size_t reverse_region_size = 1041;
    constexpr std::size_t reverse_reference_start = 9;
    const std::string reverse_subregion_target =
        std::string(reverse_reference_start, 'C')
        + reverse_subregion_full.substr(
            reverse_query_start, reverse_region_size)
        + std::string(13, 'G');
    const reference_run reverse_subregion_optimized =
        run_block_region_mode<true, true, dna4_u64>(
            reverse_subregion_source, reverse_subregion_target,
            reverse_query_start,
            reverse_query_start + reverse_region_size,
            reverse_reference_start,
            reverse_reference_start + reverse_region_size,
            scoring, params);
    const reference_run reverse_subregion_forced =
        run_block_region_mode<true, false, dna4_u64>(
            reverse_subregion_source, reverse_subregion_target,
            reverse_query_start,
            reverse_query_start + reverse_region_size,
            reverse_reference_start,
            reverse_reference_start + reverse_region_size,
            scoring, params);
    check_align_result_identical(
        reverse_subregion_optimized.result,
        reverse_subregion_forced.result);
    CHECK(reverse_subregion_optimized.stats.is_internally_consistent());
    CHECK(reverse_subregion_forced.stats.is_internally_consistent());
    CHECK(reverse_subregion_optimized.stats.simd_diagonal_certificate_calls
          == 1);
    CHECK(reverse_subregion_optimized.stats.simd_diagonal_certificate_symbols
          == reverse_region_size);
    CHECK(reverse_subregion_optimized.stats.simd_dp_cells == 0);
    CHECK(reverse_subregion_forced.stats.simd_dp_cells > 0);
    CHECK(reverse_subregion_optimized.construction.align_arena_scopes == 0);
    CHECK(reverse_subregion_optimized.construction
              .padded_sequence_allocations == 0);
    CHECK(reverse_subregion_optimized.construction.scratch_allocations == 0);
    CHECK(reverse_subregion_optimized.construction.trace_allocations == 0);
}

TEST_CASE("Packed DNA4 gap certificate handles unaligned forward and reverse windows",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);
    block_aligner_parameters params = global_params(16, 64);
    params.gap_open = 4;
    params.gap_extend = 2;
    const block_global_gap_certificate_plan plan =
        fixed_alignment_kernels<seq::dna4>().prepare_block_global_gap_certificate(scoring, params);
    REQUIRE(plan.is_contract_supported);
    REQUIRE(plan.is_generated_matrix_supported);

    constexpr std::string_view cycle = "ACGT";
    const auto patterned = [&cycle](std::size_t length, std::size_t offset) {
        std::string result(length, 'A');
        for (std::size_t i = 0; i < length; ++i) {
            result[i] = cycle[(i * 3u + i / 7u + offset) % cycle.size()];
        }
        return result;
    };

    const std::array<std::size_t, 8> lengths{
        31u, 32u, 33u, 63u, 64u, 65u, 1025u, 1057u};
    const std::array<std::size_t, 3> query_offsets{1u, 17u, 31u};
    const std::array<std::size_t, 3> target_offsets{31u, 1u, 17u};
    for (std::size_t case_index = 0; case_index < lengths.size();
         ++case_index) {
        const std::size_t length = lengths[case_index];
        const std::size_t query_start =
            query_offsets[case_index % query_offsets.size()];
        const std::size_t target_start =
            target_offsets[case_index % target_offsets.size()];
        const std::string query_source = patterned(
            query_start + length + 37u, case_index);
        std::string target_source = patterned(
            target_start + length + 29u, case_index + 11u);
        target_source.replace(
            target_start, length,
            query_source.substr(query_start, length));

        auto query_data = dna4_from_ascii(query_source);
        auto target_data = dna4_from_ascii(target_source);
        range_t query_range(query_data);
        range_t target_range(target_data);
        auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
        auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto query_view =
            bioseq::storage::make_lazy_sequence_view(lazy_query);
        const auto target_view =
            bioseq::storage::make_lazy_sequence_view(lazy_target);
        const block_packed_certificate_attempt attempt =
            fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
                false, query_view, target_view,
                query_start, query_start + length,
                target_start, target_start + length, plan);
        REQUIRE(attempt.decision
                == block_packed_certificate_decision::certified);
        CHECK(attempt.certificate.score
              == static_cast<std::int32_t>(length * 2u));
        CHECK(attempt.certificate.symbols == length);
        CHECK(attempt.certificate.vector_batches > 0u);

        const reference_run forced_dp =
            run_block_region_mode<false, false, dna4_u64>(
                query_source, target_source,
                query_start, query_start + length,
                target_start, target_start + length,
                scoring, params);
        CHECK(attempt.certificate.score == forced_dp.result.score);
        CHECK(forced_dp.result.max_q
              == static_cast<std::int32_t>(length - 1u));
        CHECK(forced_dp.result.max_t
              == static_cast<std::int32_t>(length - 1u));
        CHECK(packed_cigar_string(forced_dp.result.cigar)
              == std::to_string(length) + "M");
    }

    // Use an offset reverse-complement window that crosses packed-word and
    // 1024-symbol boundaries, then compare its score wits dynamic programming.
    constexpr std::size_t reverse_query_start = 17u;
    constexpr std::size_t reverse_target_start = 31u;
    constexpr std::size_t reverse_length = 1057u;
    const std::string reverse_query_source = patterned(
        reverse_query_start + reverse_length + 43u, 5u);
    const std::string reverse_full =
        reverse_complement_ascii(reverse_query_source);
    std::string reverse_target_source = patterned(
        reverse_target_start + reverse_length + 19u, 9u);
    reverse_target_source.replace(
        reverse_target_start, reverse_length,
        reverse_full.substr(reverse_query_start, reverse_length));
    auto reverse_query_data = dna4_from_ascii(reverse_query_source);
    auto reverse_target_data = dna4_from_ascii(reverse_target_source);
    range_t reverse_query_range(reverse_query_data);
    range_t reverse_target_range(reverse_target_data);
    auto lazy_reverse_query =
        bioseq::storage::make_resident_lazy_sequence(reverse_query_range);
    auto lazy_reverse_target =
        bioseq::storage::make_resident_lazy_sequence(reverse_target_range);
    const auto reverse_query_view =
        bioseq::storage::make_lazy_sequence_view(lazy_reverse_query);
    const auto reverse_target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_reverse_target);
    const block_packed_certificate_attempt reverse_attempt =
        fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
            true, reverse_query_view, reverse_target_view,
            reverse_query_start, reverse_query_start + reverse_length,
            reverse_target_start, reverse_target_start + reverse_length,
            plan);
    REQUIRE(reverse_attempt.decision
            == block_packed_certificate_decision::certified);
    block_aligner_execution_stats forward_certificate_stats;
    record_block_certified_global_gap(
        plan, reverse_attempt.certificate, false,
        &forward_certificate_stats);
    block_aligner_execution_stats reverse_certificate_stats;
    record_block_certified_global_gap(
        plan, reverse_attempt.certificate, true,
        &reverse_certificate_stats);
    CHECK(forward_certificate_stats.is_internally_consistent());
    CHECK(reverse_certificate_stats.is_internally_consistent());
    CHECK(forward_certificate_stats.configuration_identity_xor
          != reverse_certificate_stats.configuration_identity_xor);
    CHECK(forward_certificate_stats.configuration_identity_sum
          != reverse_certificate_stats.configuration_identity_sum);
    const reference_run reverse_forced =
        run_block_region_mode<true, false, dna4_u64>(
            reverse_query_source, reverse_target_source,
            reverse_query_start, reverse_query_start + reverse_length,
            reverse_target_start, reverse_target_start + reverse_length,
            scoring, params);
    CHECK(reverse_attempt.certificate.score == reverse_forced.result.score);
    CHECK(packed_cigar_string(reverse_forced.result.cigar)
          == std::to_string(reverse_length) + "M");

    // With these gap penalties, one substitution still proves an ungapped optimum.
    // Three substitutions fail the bound. Lower gap penalties make the one-substitution
    // case tie  the bound, so it must use dynamic programming for traceback.
    constexpr std::size_t decision_length = 65u;
    constexpr std::size_t decision_query_start = 31u;
    constexpr std::size_t decision_target_start = 17u;
    const std::string decision_query_source = patterned(140u, 3u);
    std::string decision_target_source = patterned(130u, 7u);
    decision_target_source.replace(
        decision_target_start, decision_length,
        decision_query_source.substr(
            decision_query_start, decision_length));
    decision_target_source[decision_target_start + 5u] =
        decision_target_source[decision_target_start + 5u] == 'A'
            ? 'C' : 'A';
    auto decision_query_data = dna4_from_ascii(decision_query_source);
    auto decision_target_data = dna4_from_ascii(decision_target_source);
    range_t decision_query_range(decision_query_data);
    range_t decision_target_range(decision_target_data);
    auto lazy_decision_query =
        bioseq::storage::make_resident_lazy_sequence(decision_query_range);
    auto lazy_decision_target =
        bioseq::storage::make_resident_lazy_sequence(decision_target_range);
    const auto decision_query_view =
        bioseq::storage::make_lazy_sequence_view(lazy_decision_query);
    const auto decision_target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_decision_target);
    const auto one_substitution =
        fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
            false, decision_query_view, decision_target_view,
            decision_query_start, decision_query_start + decision_length,
            decision_target_start, decision_target_start + decision_length,
            plan);
    CHECK(one_substitution.decision
          == block_packed_certificate_decision::certified);
    const reference_run one_substitution_forced =
        run_block_region_mode<false, false, dna4_u64>(
            decision_query_source, decision_target_source,
            decision_query_start,
            decision_query_start + decision_length,
            decision_target_start,
            decision_target_start + decision_length,
            scoring, params);
    CHECK(one_substitution.certificate.score
          == one_substitution_forced.result.score);
    CHECK(packed_cigar_string(one_substitution_forced.result.cigar)
          == std::to_string(decision_length) + "M");

    decision_target_source[decision_target_start + 25u] =
        decision_target_source[decision_target_start + 25u] == 'A'
            ? 'C' : 'A';
    decision_target_source[decision_target_start + 47u] =
        decision_target_source[decision_target_start + 47u] == 'A'
            ? 'C' : 'A';
    auto rejected_target_data = dna4_from_ascii(decision_target_source);
    range_t rejected_target_range(rejected_target_data);
    auto lazy_rejected_target =
        bioseq::storage::make_resident_lazy_sequence(rejected_target_range);
    const auto rejected_target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_rejected_target);
    const auto rejected =
        fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
            false, decision_query_view, rejected_target_view,
            decision_query_start, decision_query_start + decision_length,
            decision_target_start, decision_target_start + decision_length,
            plan);
    CHECK(rejected.decision == block_packed_certificate_decision::rejected);

    block_aligner_parameters tie_params = params;
    tie_params.gap_open = 1;
    tie_params.gap_extend = 1;
    const auto tie_plan = fixed_alignment_kernels<seq::dna4>().prepare_block_global_gap_certificate(
        scoring, tie_params);
    const auto tie =
        fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
            false, decision_query_view, decision_target_view,
            decision_query_start, decision_query_start + decision_length,
            decision_target_start, decision_target_start + decision_length,
            tie_plan);
    CHECK(tie.decision == block_packed_certificate_decision::rejected);
}

TEST_CASE("Packed gap executor matches forced scalar DP on a mixed production region",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    constexpr std::size_t query_length = 131u;
    constexpr std::size_t target_length = 132u;
    std::string query(query_length, 'A');
    constexpr std::string_view cycle = "ACGT";
    for (std::size_t i = 0; i < query.size(); ++i) {
        query[i] = cycle[(i * 3u + i / 5u) % cycle.size()];
    }
    std::string target = query.substr(0, 98u)
        + query.substr(98u) + "A";
    for (const std::size_t mismatch : {70u, 80u, 90u}) {
        target[mismatch] = target[mismatch] == 'A' ? 'C' : 'A';
    }
    REQUIRE(target.size() == target_length);

    auto query_data = dna4_from_ascii(query);
    auto target_data = dna4_from_ascii(target);
    range_t query_range(query_data);
    range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(lazy_target);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);
    std::vector<::bioseq::chaining::anchor> anchors{
        make_anchor_at(0u, 0u),
        make_anchor_at(32u, 32u),
        make_anchor_at(64u, 64u),
        make_anchor_at(97u, 97u),
        make_anchor_at(131u, 130u),
    };
    alignment_region fixture;
    fixture.seed_count = static_cast<std::int32_t>(anchors.size());
    fixture.anchor_start = 0;
    fixture.is_reverse = false;

    minimap2_parameters params;
    params.match_score = 2;
    params.mismatch_pen = 4;
    params.gap_open1 = 4;
    params.gap_ext1 = 2;
    params.zdrop = 100;
    params.max_gap = 64;
    params.min_ksw_len = 1;
    params.block_min_size = 16;
    params.block_max_size = 64;
    params.do_cigar = true;

    alignment_region optimized = fixture;
    block_aligner_execution_stats optimized_stats;
    align_regions<base_alignment_algorithm::block_aligner, true>(
        std::span<alignment_region>{&optimized, 1u},
        std::span<const ::bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params, &optimized_stats);
    alignment_region forced_dp = fixture;
    block_aligner_execution_stats forced_stats;
    align_regions<base_alignment_algorithm::block_aligner, false>(
        std::span<alignment_region>{&forced_dp, 1u},
        std::span<const ::bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params, &forced_stats);
    check_region_alignment_identical(optimized, forced_dp);
    CHECK(optimized_stats.is_internally_consistent());
    CHECK(forced_stats.is_internally_consistent());
    CHECK(optimized_stats.gap_executed == 4u);
    CHECK(optimized_stats.simd_diagonal_certificate_calls == 2u);
    CHECK(optimized_stats.simd_dp_cells > 0u);
    CHECK(forced_stats.gap_executed == 4u);
    CHECK(forced_stats.simd_diagonal_certificate_calls == 0u);
    CHECK(forced_stats.simd_dp_cells > optimized_stats.simd_dp_cells);

    params.do_cigar = false;
    alignment_region score_only_optimized = fixture;
    block_aligner_execution_stats score_only_optimized_stats;
    align_regions<base_alignment_algorithm::block_aligner, true>(
        std::span<alignment_region>{&score_only_optimized, 1u},
        std::span<const ::bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params,
        &score_only_optimized_stats);
    alignment_region score_only_forced = fixture;
    block_aligner_execution_stats score_only_forced_stats;
    align_regions<base_alignment_algorithm::block_aligner, false>(
        std::span<alignment_region>{&score_only_forced, 1u},
        std::span<const ::bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params,
        &score_only_forced_stats);
    check_region_alignment_identical(score_only_optimized, score_only_forced);
    CHECK(score_only_optimized.cigar.empty());
    CHECK(score_only_optimized_stats.simd_diagonal_certificate_calls == 2u);
    CHECK(score_only_optimized_stats.is_internally_consistent());
    CHECK(score_only_forced_stats.is_internally_consistent());
}

TEST_CASE("Packed gap certificate preserves fragmented DNA5 and matrix fallbacks",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);
    block_aligner_parameters params = global_params(16, 64);
    params.gap_open = 4;
    params.gap_extend = 2;
    const auto plan = fixed_alignment_kernels<seq::dna4>().prepare_block_global_gap_certificate(
        scoring, params);
    std::string sequence(1057u, 'A');
    constexpr std::string_view cycle = "ACGT";
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        sequence[i] = cycle[(i * 3u + i / 9u) % cycle.size()];
    }
    auto query_data = dna4_from_ascii(sequence);
    auto target_data = dna4_from_ascii(sequence);
    range_t query_range(query_data);
    range_t target_range(target_data);

    using chunk1 = bioseq::storage::chunked_span_sequence_scanner<
        std::uint64_t, 1>;
    bioseq::storage::lazy_sequence<dna4_u64, chunk1> fragmented_query{
        chunk1::from_range(query_range)};
    bioseq::storage::lazy_sequence<dna4_u64, chunk1> fragmented_target{
        chunk1::from_range(target_range)};
    const auto fragmented_query_view =
        bioseq::storage::make_lazy_sequence_view(fragmented_query);
    const auto fragmented_target_view =
        bioseq::storage::make_lazy_sequence_view(fragmented_target);
    const auto fragmented_attempt =
        fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
            false, fragmented_query_view, fragmented_target_view,
            0u, sequence.size(), 0u, sequence.size(), plan);
    CHECK(fragmented_attempt.decision
          == block_packed_certificate_decision::not_eligible);
    block_aligner_execution_stats fragmented_current_stats;
    const ksw_alignment_result fragmented_current =
        fixed_alignment_kernels<seq::dna4>().align_block(
            false, true, fragmented_query_view, fragmented_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &fragmented_current_stats, nullptr);
    block_aligner_execution_stats fragmented_forced_stats;
    const ksw_alignment_result fragmented_forced =
        fixed_alignment_kernels<seq::dna4>().align_block(
            false, false, fragmented_query_view, fragmented_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &fragmented_forced_stats, nullptr);
    check_align_result_identical(fragmented_current, fragmented_forced);
    CHECK(fragmented_current_stats.simd_diagonal_certificate_calls == 1u);
    CHECK(fragmented_forced_stats.simd_diagonal_certificate_calls == 0u);
    CHECK(fragmented_current_stats.is_internally_consistent());
    CHECK(fragmented_forced_stats.is_internally_consistent());

    // Fragmented reverse-complement inputs must produce the same result as forced DP.
    const std::string reverse_target_text =
        reverse_complement_ascii(sequence);
    auto reverse_target_data = dna4_from_ascii(reverse_target_text);
    range_t reverse_target_range(reverse_target_data);
    using chunk32 = bioseq::storage::chunked_span_sequence_scanner<
        std::uint64_t, 32>;
    bioseq::storage::lazy_sequence<dna4_u64, chunk32> reverse_query{
        chunk32::from_range(query_range)};
    bioseq::storage::lazy_sequence<dna4_u64, chunk32> reverse_target{
        chunk32::from_range(reverse_target_range)};
    const auto reverse_query_view =
        bioseq::storage::make_lazy_sequence_view(reverse_query);
    const auto reverse_target_view =
        bioseq::storage::make_lazy_sequence_view(reverse_target);
    const auto reverse_fragmented_attempt =
        fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
            true, reverse_query_view, reverse_target_view,
            0u, sequence.size(), 0u, sequence.size(), plan);
    CHECK(reverse_fragmented_attempt.decision
          == block_packed_certificate_decision::not_eligible);
    block_aligner_execution_stats reverse_current_stats;
    const ksw_alignment_result reverse_current =
        fixed_alignment_kernels<seq::dna4>().align_block(
            true, true, reverse_query_view, reverse_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &reverse_current_stats, nullptr);
    block_aligner_execution_stats reverse_forced_stats;
    const ksw_alignment_result reverse_forced =
        fixed_alignment_kernels<seq::dna4>().align_block(
            true, false, reverse_query_view, reverse_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &reverse_forced_stats, nullptr);
    check_align_result_identical(reverse_current, reverse_forced);
    CHECK(reverse_current_stats.simd_diagonal_certificate_calls == 1u);
    CHECK(reverse_forced_stats.simd_diagonal_certificate_calls == 0u);

    const auto dna5_scoring = scoring_matrix<dna5_u64>::generate(2, 4, 1);
    const std::string dna5_text = "ACGTNACGTACGTACGTACGTACGTACGTACG";
    std::vector<seq::dna5> dna5_data(dna5_text.size());
    for (std::size_t i = 0; i < dna5_text.size(); ++i) {
        dna5_data[i].assign_char(dna5_text[i]);
    }
    using dna5_range_t = compact_sequence_range<dna5_u64>;
    dna5_range_t dna5_query_range(dna5_data);
    dna5_range_t dna5_target_range(dna5_data);
    auto lazy_dna5_query =
        bioseq::storage::make_resident_lazy_sequence(dna5_query_range);
    auto lazy_dna5_target =
        bioseq::storage::make_resident_lazy_sequence(dna5_target_range);
    const auto dna5_query_view =
        bioseq::storage::make_lazy_sequence_view(lazy_dna5_query);
    const auto dna5_target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_dna5_target);
    const auto dna5_plan = fixed_alignment_kernels<seq::dna5>().prepare_block_global_gap_certificate(
        dna5_scoring, params);
    const auto dna5_attempt =
        fixed_alignment_kernels<seq::dna5>().try_block_certified_gap(
            false, dna5_query_view, dna5_target_view,
            0u, dna5_text.size(), 0u, dna5_text.size(), dna5_plan);
    CHECK(dna5_attempt.decision
          == block_packed_certificate_decision::not_eligible);

    auto unsupported_scoring = scoring;
    unsupported_scoring.at(0, 1) = 3;
    unsupported_scoring.simd_scores[1] = 3;
    REQUIRE(unsupported_scoring.is_block_layout_valid());
    const auto unsupported_plan =
        fixed_alignment_kernels<seq::dna4>().prepare_block_global_gap_certificate(
            unsupported_scoring, params);
    CHECK_FALSE(unsupported_plan.is_generated_matrix_supported);
    auto resident_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto resident_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto resident_query_view =
        bioseq::storage::make_lazy_sequence_view(resident_query);
    const auto resident_target_view =
        bioseq::storage::make_lazy_sequence_view(resident_target);
    const auto unsupported_attempt =
        fixed_alignment_kernels<seq::dna4>().try_block_certified_gap(
            false, resident_query_view, resident_target_view,
            0u, sequence.size(), 0u, sequence.size(), unsupported_plan);
    CHECK(unsupported_attempt.decision
          == block_packed_certificate_decision::not_eligible);
}

} // namespace bioseq::simd
