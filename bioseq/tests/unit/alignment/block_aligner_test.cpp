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
    const auto nw1 = scoring_matrix<dna4_u64>::generate(1, 1, 1).value();

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
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1).value();
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
    std::array<std::int32_t, scoring_matrix<dna4_u64>::k_entry_count> dense_entries{};
    std::ranges::copy(scoring.score_entries(), dense_entries.begin());
    dense_entries[1] = 3;
    const auto unsupported_scoring = scoring_matrix<dna4_u64>::from_entries(dense_entries).value();
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

    const auto dna5_scoring = scoring_matrix<dna5_u64>::generate(2, 4, 1).value();
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
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1).value();
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
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1).value();
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
        fixed_alignment_kernels<seq::dna4>(), params, &optimized_stats).value();
    alignment_region forced_dp = fixture;
    block_aligner_execution_stats forced_stats;
    align_regions<base_alignment_algorithm::block_aligner, false>(
        std::span<alignment_region>{&forced_dp, 1u},
        std::span<const ::bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params, &forced_stats).value();
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
        &score_only_optimized_stats).value();
    alignment_region score_only_forced = fixture;
    block_aligner_execution_stats score_only_forced_stats;
    align_regions<base_alignment_algorithm::block_aligner, false>(
        std::span<alignment_region>{&score_only_forced, 1u},
        std::span<const ::bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params,
        &score_only_forced_stats).value();
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
    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1).value();
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
            &fragmented_current_stats, nullptr).value();
    block_aligner_execution_stats fragmented_forced_stats;
    const ksw_alignment_result fragmented_forced =
        fixed_alignment_kernels<seq::dna4>().align_block(
            false, false, fragmented_query_view, fragmented_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &fragmented_forced_stats, nullptr).value();
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
            &reverse_current_stats, nullptr).value();
    block_aligner_execution_stats reverse_forced_stats;
    const ksw_alignment_result reverse_forced =
        fixed_alignment_kernels<seq::dna4>().align_block(
            true, false, reverse_query_view, reverse_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &reverse_forced_stats, nullptr).value();
    check_align_result_identical(reverse_current, reverse_forced);
    CHECK(reverse_current_stats.simd_diagonal_certificate_calls == 1u);
    CHECK(reverse_forced_stats.simd_diagonal_certificate_calls == 0u);

    const auto dna5_scoring = scoring_matrix<dna5_u64>::generate(2, 4, 1).value();
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
    // Resident DNA5 uses the generic native packed certificate path. N is a
    // biological rank here; the matrix's separate padding rank is not an input.
    REQUIRE(dna5_plan.is_contract_supported);
    REQUIRE(dna5_plan.is_generated_matrix_supported);
    REQUIRE(dna5_attempt.decision
            == block_packed_certificate_decision::certified);
    CHECK(dna5_attempt.certificate.is_certified);
    CHECK(dna5_attempt.certificate.symbols == dna5_text.size());
    CHECK(dna5_attempt.certificate.edit_distance == 0u);
    CHECK(dna5_attempt.certificate.score
          == static_cast<std::int32_t>(dna5_text.size() * 2u));
    CHECK(dna5_attempt.certificate.vector_batches > 0u);

    const reference_run dna5_optimized = run_block_mode<false, true, dna5_u64>(
        dna5_text, dna5_text, dna5_scoring, params);
    const reference_run dna5_forced_dp = run_block_mode<false, false, dna5_u64>(
        dna5_text, dna5_text, dna5_scoring, params);
    check_align_result_identical(dna5_optimized.result, dna5_forced_dp.result);
    CHECK(dna5_optimized.result.score == dna5_attempt.certificate.score);
    CHECK(dna5_optimized.stats.simd_diagonal_certificate_calls == 1u);
    CHECK(dna5_forced_dp.stats.simd_diagonal_certificate_calls == 0u);
    CHECK(dna5_forced_dp.stats.simd_dp_cells > 0u);
    CHECK(dna5_optimized.stats.is_internally_consistent());
    CHECK(dna5_forced_dp.stats.is_internally_consistent());

    std::array<std::int32_t, scoring_matrix<dna4_u64>::k_entry_count> dense_entries{};
    std::ranges::copy(scoring.score_entries(), dense_entries.begin());
    dense_entries[1] = 3;
    const auto unsupported_scoring = scoring_matrix<dna4_u64>::from_entries(dense_entries).value();
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

// BEGIN_TEMPORARY_NATIVE_PROTEIN_PROBE — DO NOT COMMIT
// Temporary, uncommitted probe. Insert immediately before the closing
// namespace bioseq::simd in bioseq/tests/unit/alignment/block_aligner_test.cpp.
// Existing imports and block_aligner_test_support provide every dependency.
// Focused Catch expression: "Manual native AA27 CPU scoring probe"
// Remove this block after the MCP-managed manual run; do not commit it.
namespace manual_protein_alignment_probe {

using alphabet = seq::aa27;
using packed_source = sequence_custom_alphabet<alphabet, std::uint64_t>;
using packed_range = compact_sequence_range<packed_source>;
using matrix_type = scoring_matrix<alphabet>;

/** Construct native SeqAn ranks directly, including ranks above the first byte table. */
template<sequence_alphabet Alphabet = alphabet>
[[nodiscard]] std::vector<Alphabet> letters(std::span<const std::uint8_t> ranks) {
    std::vector<Alphabet> output;
    output.reserve(ranks.size());
    for (const auto rank : ranks) {
        REQUIRE(rank < seqan3::alphabet_size<Alphabet>);
        output.push_back(seqan3::assign_rank_to(rank, Alphabet{}));
    }
    return output;
}

/** Independent O(T)-memory scalar Gotoh oracle; no existing scalar affine oracle was found. */
template<class Matrix>
[[nodiscard]] std::int64_t affine_oracle(
    std::span<const std::uint8_t> query, std::span<const std::uint8_t> target,
    const Matrix& matrix, std::int32_t gap_open, std::int32_t gap_extend) {
    constexpr auto unreachable = std::numeric_limits<std::int64_t>::min() / 4;
    std::vector<std::int64_t> prior(target.size() + 1), current(target.size() + 1);
    std::vector<std::int64_t> insertions(target.size() + 1, unreachable);
    for (std::size_t column = 1; column <= target.size(); ++column)
        prior[column] = -std::int64_t{gap_open} - std::int64_t{gap_extend} * static_cast<std::int64_t>(column);
    for (std::size_t row = 1; row <= query.size(); ++row) {
        current[0] = -std::int64_t{gap_open} - std::int64_t{gap_extend} * static_cast<std::int64_t>(row);
        auto deletion = unreachable;
        for (std::size_t column = 1; column <= target.size(); ++column) {
            insertions[column] = std::max(prior[column] - gap_open - gap_extend,
                                          insertions[column] - gap_extend);
            deletion = std::max(current[column - 1] - gap_open - gap_extend,
                                deletion - gap_extend);
            const auto diagonal = prior[column - 1]
                + matrix.at(target[column - 1], query[row - 1]);
            current[column] = std::max({diagonal, insertions[column], deletion});
        }
        prior.swap(current);
    }
    return prior.back();
}

/** Replay the returned native CIGAR against the same ranks and independently charge each run. */
template<class Matrix>
void check_cigar(const ksw_alignment_result& result,
    std::span<const std::uint8_t> query, std::span<const std::uint8_t> target,
    const Matrix& matrix, std::int32_t gap_open, std::int32_t gap_extend) {
    std::vector<bioseq::cigar_run> runs;
    REQUIRE(bioseq::decode_native_cigar(result.cigar, runs).has_value());
    std::size_t query_position = 0, target_position = 0;
    std::int64_t score = 0;
    for (const auto& run : runs) {
        const auto operation = seqan3::to_char(run.operation);
        INFO("CIGAR operation=" << operation << " length=" << run.length);
        if (operation == 'M' || operation == '=' || operation == 'X') {
            REQUIRE(run.length <= query.size() - query_position);
            REQUIRE(run.length <= target.size() - target_position);
            for (std::size_t offset = 0; offset < run.length; ++offset)
                score += matrix.at(target[target_position + offset], query[query_position + offset]);
            query_position += run.length;
            target_position += run.length;
        } else if (operation == 'I') {
            REQUIRE(run.length <= query.size() - query_position);
            query_position += run.length;
            score -= std::int64_t{gap_open} + std::int64_t{gap_extend} * run.length;
        } else if (operation == 'D') {
            REQUIRE(run.length <= target.size() - target_position);
            target_position += run.length;
            score -= std::int64_t{gap_open} + std::int64_t{gap_extend} * run.length;
        } else {
            FAIL("Unexpected global-alignment CIGAR operation");
        }
    }
    CHECK(query_position == query.size());
    CHECK(target_position == target.size());
    CHECK(score == result.score);
}

/** Exercise the existing fixed-target tables, without constructing another alignment wrapper. */
void run_case(std::string_view label,
    const std::vector<std::uint8_t>& query, const std::vector<std::uint8_t>& target,
    const matrix_type& matrix, std::int64_t expected,
    std::size_t minimum_block = 16, std::size_t maximum_block = 64,
    bool include_dominated_dual = true) {
    INFO("case=" << label << " query=" << query.size() << " target=" << target.size());
    constexpr std::int32_t gap_open = 4, gap_extend = 2;
    const auto oracle = affine_oracle(query, target, matrix, gap_open, gap_extend);
    REQUIRE(oracle == expected);
    packed_range query_range(letters(query));
    packed_range target_range(letters(target));
    auto query_owner = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto target_owner = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(query_owner);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(target_owner);
    const auto& kernels = fixed_alignment_kernels<alphabet>();
    ksw_single_affine_parameters single;
    single.gap_open = gap_open;
    single.gap_extend = gap_extend;
    single.band_width = -1;
    single.z_drop = -1;
    const auto ksw = kernels.align_ksw2_single_affine(false, query_view, target_view,
        0, query.size(), 0, target.size(), matrix, single);
    REQUIRE(ksw.has_value());
    CHECK(ksw->score == oracle);
    check_cigar(*ksw, query, target, matrix, gap_open, gap_extend);

    if (include_dominated_dual) {
        ksw_dual_affine_parameters dual;
        dual.gap_open_short = gap_open;
        dual.gap_extend_short = gap_extend;
        dual.gap_open_long = 1000;
        dual.gap_extend_long = 1000;
        dual.band_width = -1;
        dual.z_drop = -1;
        const auto dominated = kernels.align_ksw2_dual_affine(false, query_view, target_view,
            0, query.size(), 0, target.size(), matrix, dual);
        REQUIRE(dominated.has_value());
        CHECK(dominated->score == oracle);
        CHECK(dominated->cigar == ksw->cigar);
        check_cigar(*dominated, query, target, matrix, gap_open, gap_extend);
    }

    auto parameters = global_params(minimum_block, maximum_block);
    parameters.gap_open = gap_open;
    parameters.gap_extend = gap_extend;
    block_aligner_execution_stats stats;
    block_aligner_construction_stats construction;
    const auto block = kernels.align_block(false, false, query_view, target_view,
        0, query.size(), 0, target.size(), matrix, parameters, &stats, &construction);
    REQUIRE(block.has_value());
    CHECK(block->score == oracle);
    CHECK(block->max_q == static_cast<std::int32_t>(query.size()) - 1);
    CHECK(block->max_t == static_cast<std::int32_t>(target.size()) - 1);
    CHECK(stats.is_internally_consistent());
    CHECK(stats.simd_diagonal_certificate_calls == 0);
    CHECK(stats.forbidden_ksw2_calls == 0);
    CHECK(stats.forbidden_scalar_biological_fallbacks == 0);
    if (!query.empty() && !target.empty()) CHECK(stats.simd_dp_vector_batches > 0);
    check_cigar(*block, query, target, matrix, gap_open, gap_extend);
}
} // namespace manual_protein_alignment_probe

// BEGIN_TEMPORARY_REGISTERED_CPU_SCORING_ORACLE
namespace manual_protein_alignment_probe {

/** Reuse the independent Gotoh oracle across the registered native scoring layouts. */
template<sequence_alphabet Alphabet>
void run_registered_view_case(std::string_view label,
    const std::vector<std::uint8_t>& query, const std::vector<std::uint8_t>& target,
    const scoring_matrix<Alphabet>& matrix,
    bioseq::storage::lazy_sequence_view<Alphabet> query_view,
    bioseq::storage::lazy_sequence_view<Alphabet> target_view,
    std::size_t& oracle_cases, std::size_t& alignment_calls) {
    INFO("alphabet=" << static_cast<std::size_t>(supported_biological_alphabets::template index_of<Alphabet>())
        << " cardinality=" << std::size_t{seqan3::alphabet_size<Alphabet>}
        << " matrix_kind=" << static_cast<unsigned>(matrix.kind())
        << " case=" << label << " query=" << query.size() << " target=" << target.size());
    constexpr std::int32_t gap_open = 4, gap_extend = 2;
    const auto expected = affine_oracle(query, target, matrix, gap_open, gap_extend);
    ++oracle_cases;
    const auto& kernels = fixed_alignment_kernels<Alphabet>();

    for (const bool trace : {true, false}) {
        CAPTURE(trace);
        ksw_single_affine_parameters single;
        single.gap_open = gap_open;
        single.gap_extend = gap_extend;
        single.band_width = -1;
        single.z_drop = -1;
        single.flags.score_only = !trace;
        const auto single_result = kernels.align_ksw2_single_affine(false,
            query_view, target_view, 0u, query.size(), 0u, target.size(), matrix, single);
        ++alignment_calls;
        REQUIRE(single_result.has_value());
        CHECK(single_result->score == expected);
        if (trace) check_cigar(*single_result, query, target, matrix, gap_open, gap_extend);
        else {
            CHECK(single_result->cigar.empty());
            CHECK(single_result->edit_distance == 0u);
            CHECK_FALSE(single_result->is_edit_distance_valid);
        }

        // Both curves normalize to the oracle's single affine cost for every positive gap length.
        ksw_dual_affine_parameters dual;
        dual.gap_open_short = gap_open;
        dual.gap_extend_short = gap_extend;
        dual.gap_open_long = 1000;
        dual.gap_extend_long = 1000;
        dual.band_width = -1;
        dual.z_drop = -1;
        dual.flags.score_only = !trace;
        const auto dual_result = kernels.align_ksw2_dual_affine(false,
            query_view, target_view, 0u, query.size(), 0u, target.size(), matrix, dual);
        ++alignment_calls;
        REQUIRE(dual_result.has_value());
        CHECK(dual_result->score == expected);
        if (trace) check_cigar(*dual_result, query, target, matrix, gap_open, gap_extend);
        else {
            CHECK(dual_result->cigar.empty());
            CHECK(dual_result->edit_distance == 0u);
            CHECK_FALSE(dual_result->is_edit_distance_valid);
        }

        auto parameters = global_params(16u, 128u, trace);
        parameters.gap_open = gap_open;
        parameters.gap_extend = gap_extend;
        block_aligner_execution_stats stats;
        block_aligner_construction_stats construction;
        const auto block_result = kernels.align_block(false, false, query_view, target_view,
            0u, query.size(), 0u, target.size(), matrix, parameters, &stats, &construction);
        ++alignment_calls;
        REQUIRE(block_result.has_value());
        CHECK(block_result->score == expected);
        CHECK(block_result->max_q == static_cast<std::int32_t>(query.size()) - 1);
        CHECK(block_result->max_t == static_cast<std::int32_t>(target.size()) - 1);
        CHECK(stats.is_internally_consistent());
        CHECK(stats.forbidden_ksw2_calls == 0u);
        CHECK(stats.forbidden_scalar_biological_fallbacks == 0u);
        if (trace) check_cigar(*block_result, query, target, matrix, gap_open, gap_extend);
        else {
            CHECK(block_result->cigar.empty());
            CHECK(block_result->edit_distance == 0u);
            CHECK_FALSE(block_result->is_edit_distance_valid);
            CHECK(stats.trace_calls == 0u);
            CHECK(construction.trace_allocations == 0u);
        }
    }
}

/** Keep width-specific source ownership outside the Alphabet-only numerical runner. */
template<sequence_alphabet Alphabet, std::unsigned_integral Unit>
void run_registered_case(std::string_view label,
    const std::vector<std::uint8_t>& query, const std::vector<std::uint8_t>& target,
    const scoring_matrix<Alphabet>& matrix,
    std::size_t& oracle_cases, std::size_t& alignment_calls) {
    INFO("storage_bits=" << std::numeric_limits<Unit>::digits);
    using range_type = compact_sequence_range<sequence_custom_alphabet<Alphabet, Unit>>;
    using traits = typename range_type::traits;
    range_type query_range(letters<Alphabet>(query));
    range_type target_range(letters<Alphabet>(target));
    // Unused physical bits and final slots may contain invalid ranks; no path may consume them.
    const auto poison_padding = [](range_type& range) {
        for (auto& unit : range.packed_units)
            unit |= static_cast<Unit>(~traits::used_storage_mask);
        for (std::size_t slot = traits::letters_per_unit - range.padding();
             slot < traits::letters_per_unit; ++slot)
            range.packed_units.back() |= static_cast<Unit>(
                traits::char_mask << (slot * traits::bits_per_char));
    };
    poison_padding(query_range);
    poison_padding(target_range);
    auto query_owner = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto target_owner = bioseq::storage::make_resident_lazy_sequence(target_range);
    run_registered_view_case<Alphabet>(label, query, target, matrix,
        bioseq::storage::make_lazy_sequence_view(query_owner),
        bioseq::storage::make_lazy_sequence_view(target_owner), oracle_cases, alignment_calls);
}

/** Derive every rank, physical tail, and matrix stride from the selected alphabet. */
template<sequence_alphabet Alphabet, std::unsigned_integral Unit>
void run_registered_width(const scoring_matrix<Alphabet>& uniform,
    const scoring_matrix<Alphabet>& dense,
    std::size_t& oracle_cases, std::size_t& alignment_calls) {
    using range_type = compact_sequence_range<sequence_custom_alphabet<Alphabet, Unit>>;
    using traits = typename range_type::traits;
    constexpr std::size_t rank_count = seqan3::alphabet_size<Alphabet>;
    constexpr auto highest = static_cast<std::uint8_t>(rank_count - 1u);
    std::size_t logical_size = 2u * rank_count + 1u;
    if constexpr (traits::letters_per_unit > 1u)
        if (logical_size % traits::letters_per_unit == 0u) ++logical_size;
    std::vector<std::uint8_t> native_ranks(logical_size);
    for (std::size_t position = 0u; position < logical_size; ++position)
        native_ranks[position] = static_cast<std::uint8_t>(position % rank_count);
    if constexpr (traits::letters_per_unit > 1u)
        REQUIRE(logical_size % traits::letters_per_unit != 0u);
    auto longer = native_ranks;
    longer.insert(longer.begin() + static_cast<std::ptrdiff_t>(logical_size / 2u),
        3u, highest);
    const std::vector<std::uint8_t> empty;
    const std::vector<std::uint8_t> low{std::uint8_t{0u}};
    const std::vector<std::uint8_t> high{highest};
    for (const bool use_dense : {false, true}) {
        const auto& prepared = use_dense ? dense : uniform;
        run_registered_case<Alphabet, Unit>("all native ranks with physical tail bits", native_ranks, native_ranks,
            prepared, oracle_cases, alignment_calls);
        run_registered_case<Alphabet, Unit>("target high query low", low, high,
            prepared, oracle_cases, alignment_calls);
        run_registered_case<Alphabet, Unit>("target low query high", high, low,
            prepared, oracle_cases, alignment_calls);
        run_registered_case<Alphabet, Unit>("longer query", longer, native_ranks,
            prepared, oracle_cases, alignment_calls);
        run_registered_case<Alphabet, Unit>("longer target", native_ranks, longer,
            prepared, oracle_cases, alignment_calls);
        run_registered_case<Alphabet, Unit>("empty query", empty, high,
            prepared, oracle_cases, alignment_calls);
        run_registered_case<Alphabet, Unit>("empty target", high, empty,
            prepared, oracle_cases, alignment_calls);
        run_registered_case<Alphabet, Unit>("both empty", empty, empty,
            prepared, oracle_cases, alignment_calls);
    }
}
/** Prepare each scoring layout once per alphabet and reuse it across packed word widths. */
template<sequence_alphabet Alphabet>
void run_registered_alphabet(std::size_t& oracle_cases, std::size_t& alignment_calls) {
    using matrix = scoring_matrix<Alphabet>;
    constexpr std::size_t rank_count = seqan3::alphabet_size<Alphabet>;
    constexpr std::size_t dimension = matrix::k_alphabet_dimension;
    const auto uniform = matrix::generate(3, 5, 1);
    REQUIRE(uniform.has_value());
    REQUIRE(uniform->kind() == substitution_kind::uniform);
    std::array<std::int32_t, matrix::k_entry_count> entries{};
    for (std::size_t target_rank = 0u; target_rank < rank_count; ++target_rank)
        for (std::size_t query_rank = 0u; query_rank < rank_count; ++query_rank)
            entries[target_rank * dimension + query_rank] = target_rank == query_rank
                ? 3 + static_cast<std::int32_t>(target_rank % 3u) : -5;
    entries[(rank_count - 1u) * dimension] = 1;
    entries[rank_count - 1u] = -6;
    const auto dense = matrix::from_entries(entries);
    REQUIRE(dense.has_value());
    REQUIRE(dense->kind() == substitution_kind::dense);
    REQUIRE(dense->at(rank_count - 1u, 0u) == 1);
    REQUIRE(dense->at(0u, rank_count - 1u) == -6);
    using entry = supported_biological_alphabets::entry_for<Alphabet>;
    using header = compact_sequence_stdc<supported_alphabets<entry>>;
    [&uniform, &dense, &oracle_cases, &alignment_calls]<std::size_t... Widths>(
            supported_storage_unit_byte_widths<Widths...>) {
        (run_registered_width<Alphabet,
             typename compact_sequence_stdc_instance<header, Alphabet, Widths>::storage_unit>(
                 *uniform, *dense, oracle_cases, alignment_calls), ...);
    }(all_storage_unit_byte_widths{});
}

/** Add the registry/word-width matrix to the existing temporary AA27 test case. */
void run_registered_matrix() {
    std::size_t oracle_cases = 0u;
    std::size_t alignment_calls = 0u;
    [&oracle_cases, &alignment_calls]<class... Entries>(std::tuple<Entries...>) {
        (run_registered_alphabet<typename Entries::alphabet_type>(oracle_cases, alignment_calls), ...);
    }(supported_biological_alphabets::alphabet_entries_tuple{});
    constexpr std::size_t alphabet_count =
        std::tuple_size_v<supported_biological_alphabets::alphabet_entries_tuple>;
    CHECK(oracle_cases == alphabet_count * all_storage_unit_byte_widths::width_count * 2u * 8u);
    CHECK(alignment_calls == oracle_cases * 2u * 3u);
}
} // namespace manual_protein_alignment_probe
// END_TEMPORARY_REGISTERED_CPU_SCORING_ORACLE

TEST_CASE("Manual native AA27 CPU scoring probe",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    namespace probe = manual_protein_alignment_probe;
    constexpr auto rank_count = seqan3::alphabet_size<probe::alphabet>;
    constexpr auto dimension = probe::matrix_type::k_alphabet_dimension;
    REQUIRE(rank_count == 27); // This manual fixture deliberately exercises ranks 16..26.
    std::array<std::int32_t, probe::matrix_type::k_entry_count> entries{};
    for (std::size_t target = 0; target < rank_count; ++target)
        for (std::size_t query = 0; query < rank_count; ++query)
            entries[target * dimension + query] = target == query
                ? 2 + static_cast<std::int32_t>(target % 5) : -4;
    entries[26 * dimension + 16] = 1;
    entries[16 * dimension + 26] = -5;
    const auto prepared = probe::matrix_type::from_entries(entries);
    REQUIRE(prepared.has_value());
    const auto& matrix = *prepared;
    REQUIRE(matrix.kind() == substitution_kind::dense);
    REQUIRE(matrix.at(rank_count, rank_count) == 0);
    REQUIRE(matrix.summary().maximum == 6);
    REQUIRE(matrix.at(0, 0) == 2);

    std::vector<std::uint8_t> high_ranks;
    for (std::size_t repeat = 0; repeat < 4; ++repeat)
        for (std::size_t rank = 16; rank < rank_count; ++rank)
            high_ranks.push_back(static_cast<std::uint8_t>(rank));
    probe::run_case("all high ranks", high_ranks, high_ranks, matrix, 172);
    probe::run_case("asymmetric target26 query16", {16}, {26}, matrix, 1);
    probe::run_case("asymmetric target16 query26", {26}, {16}, matrix, -5);
    auto longer = high_ranks;
    longer.insert(longer.begin() + 22, 3, std::uint8_t{26});
    probe::run_case("longer query", longer, high_ranks, matrix, 162);
    probe::run_case("longer target", high_ranks, longer, matrix, 162);
    probe::run_case("empty query", {}, {16,17,18,19,20}, matrix, -14);
    probe::run_case("empty target", {16,17,18,19,20}, {}, matrix, -14);
    probe::run_case("both empty", {}, {}, matrix, 0);

    // 3072*6 exceeds the positive headroom above Block's initial bias16384.
    // Correct full score therefore requires at least one successful frontier rebase.
    const std::vector<std::uint8_t> long_target(3072, std::uint8_t{24});
    auto long_query = long_target;
    long_query.insert(long_query.begin() + 1536, 128, std::uint8_t{26});
    probe::run_case("long insertion and rebase", long_query, long_target,
        matrix, 18172, 16, 256, false);
    probe::run_case("long deletion and rebase", long_target, long_query,
        matrix, 18172, 16, 256, false);
    // BEGIN_TEMPORARY_REGISTERED_CPU_SCORING_ORACLE_CALL
    probe::run_registered_matrix();
    // END_TEMPORARY_REGISTERED_CPU_SCORING_ORACLE_CALL
}

// Temporary, uncommitted addition to the existing manual native CPU probe.
// Append inside namespace bioseq::simd in tests/unit/alignment/block_aligner_test.cpp,
// after /tmp/bioseq_native_cpu_probe.cpp, whose native fixtures/oracle are reused.
// Catch expression: "Manual native CPU dual gap normalization probe"
// Root executes only through bioseq-build MCP; remove before committing.
TEST_CASE("Manual native CPU dual gap normalization probe",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context storage;
    context::installed_context_scope installed{storage};
    namespace probe = manual_protein_alignment_probe;
    const auto matrix = probe::matrix_type::generate(2, 4, 1);
    REQUIRE(matrix.has_value());
    constexpr auto highest = static_cast<std::uint8_t>(seqan3::alphabet_size<probe::alphabet> - 1u);
    const std::array<ksw_dual_affine_parameters, 3> curves{{
        // The second pair dominates for all integer gap lengths, despite greater q.
        {.gap_open_short=0, .gap_extend_short=4, .gap_open_long=2, .gap_extend_long=1,
         .band_width=-1, .z_drop=-1},
        // Equal one-symbol costs still collapse to the lower extension slope.
        {.gap_open_short=0, .gap_extend_short=3, .gap_open_long=2, .gap_extend_long=1,
         .band_width=-1, .z_drop=-1},
        // Genuine crossing: short wins length1, long wins length4.
        {.gap_open_short=0, .gap_extend_short=2, .gap_open_long=3, .gap_extend_long=1,
         .band_width=-1, .z_drop=-1},
    }};
    std::size_t alignment_cases = 0u;
    for (std::size_t curve_index=0; curve_index<curves.size(); ++curve_index) {
        for (const auto gap_length : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            const auto& curve = curves[curve_index];
            const auto first_cost = std::int64_t{curve.gap_open_short}
                + static_cast<std::int64_t>(gap_length) * curve.gap_extend_short;
            const auto second_cost = std::int64_t{curve.gap_open_long}
                + static_cast<std::int64_t>(gap_length) * curve.gap_extend_long;
            const bool first_wins = first_cost <= second_cost;
            const auto selected_open = first_wins ? curve.gap_open_short : curve.gap_open_long;
            const auto selected_extend = first_wins ? curve.gap_extend_short : curve.gap_extend_long;
            const auto expected = std::int64_t{2} - std::min(first_cost, second_cost);
            const std::vector<std::uint8_t> short_ranks{highest};
            std::vector<std::uint8_t> long_ranks(gap_length, static_cast<std::uint8_t>(highest - 1u));
            long_ranks.push_back(highest); // Optimal gap must precede the sole matching residue.
            for (const bool longer_query : {false, true}) {
                const auto& query = longer_query ? long_ranks : short_ranks;
                const auto& target = longer_query ? short_ranks : long_ranks;
                CAPTURE(curve_index, gap_length, longer_query);
                // Here every optimal path has one leading gap and the sole equal pair.
                // The independent affine oracle for that gap's cheaper pair proves the score.
                REQUIRE(probe::affine_oracle(query, target, *matrix,
                    selected_open, selected_extend) == expected);
                auto query_letters = probe::letters(query);
                auto target_letters = probe::letters(target);
                probe::packed_range query_range{query_letters};
                probe::packed_range target_range{target_letters};
                auto query_owner = bioseq::storage::make_resident_lazy_sequence(query_range);
                auto target_owner = bioseq::storage::make_resident_lazy_sequence(target_range);
                const auto query_view = bioseq::storage::make_lazy_sequence_view(query_owner);
                const auto target_view = bioseq::storage::make_lazy_sequence_view(target_owner);
                for (const bool swap_pairs : {false, true}) {
                    CAPTURE(swap_pairs);
                    auto parameters = curve;
                    if (swap_pairs) {
                        std::swap(parameters.gap_open_short, parameters.gap_open_long);
                        std::swap(parameters.gap_extend_short, parameters.gap_extend_long);
                    }
                    const auto aligned = fixed_alignment_kernels<probe::alphabet>().align_ksw2_dual_affine(
                        false, query_view, target_view, 0u, query.size(), 0u, target.size(), *matrix, parameters);
                    ++alignment_cases;
                    REQUIRE(aligned.has_value());
                    CHECK(aligned->score == expected);
                    probe::check_cigar(*aligned, query, target, *matrix, selected_open, selected_extend);
                }
            }
        }
    }
    CHECK(alignment_cases == 36u);
}

// END_TEMPORARY_NATIVE_PROTEIN_PROBE

} // namespace bioseq::simd
