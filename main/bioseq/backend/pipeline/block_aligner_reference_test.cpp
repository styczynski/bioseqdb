// Golden differential tests for the Highway Block Aligner port.
//
// The expected values below are copied from the local upstream research tree:
//   block-aligner-research/src/scan_block.rs::tests
// They intentionally compare the native single-affine Global/XDrop contract,
// not KSW2 z-drop or dual-affine behaviour.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.sequences.minimap_port;
import bioseq.sequences.chaining.utils;
import bioseq.common.context;
import bioseq.storage.lazy;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = compact_sequence_range<dna4_u64>;
using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;

struct ReferenceRun {
    KswAlignResult result;
    BlockAlignerExecutionProof proof;
};

[[nodiscard]] std::vector<seq::dna4> dna4_from_ascii(
        std::string_view text) {
    std::vector<seq::dna4> result(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        result[i].assign_char(text[i]);
    }
    return result;
}

[[nodiscard]] std::string packed_cigar_string(
        std::span<const std::uint32_t> cigar) {
    std::string result;
    for (const std::uint32_t packed : cigar) {
        result += std::to_string(packed >> 4u);
        switch (packed & 0xfu) {
        case CIGAR_MATCH: result += 'M'; break;
        case CIGAR_INS: result += 'I'; break;
        case CIGAR_DEL: result += 'D'; break;
        default: result += '?'; break;
        }
    }
    return result;
}

[[nodiscard]] BlockAlignerParams global_params(
        std::size_t min_size = 16,
        std::size_t max_size = 16,
        bool trace = true) {
    return BlockAlignerParams{
        .gap_open = 1,
        .gap_extend = 1,
        .min_size = min_size,
        .max_size = max_size,
        .x_drop = 0,
        .mode = BlockAlignmentMode::Global,
        .input_order = BlockInputOrder::Forward,
        .phase = BlockAlignerPhase::GapExtension,
        .trace = trace,
    };
}

[[nodiscard]] BlockAlignerParams xdrop_params(
        BlockAlignerPhase phase,
        BlockInputOrder input_order,
        bool trace = true) {
    return BlockAlignerParams{
        .gap_open = 10,
        .gap_extend = 1,
        .min_size = 16,
        .max_size = 16,
        .x_drop = 1,
        .mode = BlockAlignmentMode::XDrop,
        .input_order = input_order,
        .phase = phase,
        .trace = trace,
    };
}

[[nodiscard]] ReferenceRun run_block(
        std::string_view query,
        std::string_view reference,
        const ScoringMatrix<dna4_u64>& scoring,
        const BlockAlignerParams& params) {
    auto query_data = dna4_from_ascii(query);
    auto reference_data = dna4_from_ascii(reference);
    range_t query_range(query_data);
    range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_reference = bioseq::storage::make_lazy_in_memory(reference_range);

    ReferenceRun run;
    run.result = HwyBlockAligner::align<false>(
        lazy_query, lazy_reference,
        0, query.size(), 0, reference.size(), scoring, params, &run.proof);
    return run;
}

template<bool strand_reversed, bool enable_diagonal_certificate, class Sequence>
[[nodiscard]] ReferenceRun run_block_mode(
        std::string_view query,
        std::string_view reference,
        const ScoringMatrix<Sequence>& scoring,
        const BlockAlignerParams& params) {
    using typed_range_t = compact_sequence_range<Sequence>;
    using alphabet_t = typename typed_range_t::traits::alphabet_type;
    std::vector<alphabet_t> query_data(query.size());
    std::vector<alphabet_t> reference_data(reference.size());
    for (std::size_t i = 0; i < query.size(); ++i) {
        query_data[i].assign_char(query[i]);
    }
    for (std::size_t i = 0; i < reference.size(); ++i) {
        reference_data[i].assign_char(reference[i]);
    }
    typed_range_t query_range(query_data);
    typed_range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_reference = bioseq::storage::make_lazy_in_memory(reference_range);

    ReferenceRun run;
    run.result = HwyBlockAligner::align<
        strand_reversed, enable_diagonal_certificate>(
            lazy_query, lazy_reference,
            0, query.size(), 0, reference.size(), scoring, params, &run.proof);
    return run;
}

void check_align_result_identical(
        const KswAlignResult& optimized,
        const KswAlignResult& forced_dp) {
    CHECK(optimized.max == forced_dp.max);
    CHECK(optimized.score == forced_dp.score);
    CHECK(optimized.mqe == forced_dp.mqe);
    CHECK(optimized.mte == forced_dp.mte);
    CHECK(optimized.max_q == forced_dp.max_q);
    CHECK(optimized.max_t == forced_dp.max_t);
    CHECK(optimized.mqe_t == forced_dp.mqe_t);
    CHECK(optimized.mte_q == forced_dp.mte_q);
    CHECK(optimized.zdropped == forced_dp.zdropped);
    CHECK(optimized.reach_end == forced_dp.reach_end);
    CHECK(optimized.cigar == forced_dp.cigar);
}

[[nodiscard]] std::string reverse_complement_ascii(std::string_view source) {
    std::string result(source.rbegin(), source.rend());
    for (char& symbol : result) {
        switch (symbol) {
        case 'A': symbol = 'T'; break;
        case 'C': symbol = 'G'; break;
        case 'G': symbol = 'C'; break;
        case 'T': symbol = 'A'; break;
        default: break;
        }
    }
    return result;
}

[[nodiscard]] ReferenceRun run_block_region(
        std::string_view query_source,
        std::string_view reference_source,
        std::size_t query_start,
        std::size_t query_end,
        std::size_t reference_start,
        std::size_t reference_end,
        const ScoringMatrix<dna4_u64>& scoring,
        const BlockAlignerParams& params) {
    auto query_data = dna4_from_ascii(query_source);
    auto reference_data = dna4_from_ascii(reference_source);
    range_t query_range(query_data);
    range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_reference = bioseq::storage::make_lazy_in_memory(reference_range);

    ReferenceRun run;
    run.result = HwyBlockAligner::align<false>(
        lazy_query, lazy_reference,
        query_start, query_end, reference_start, reference_end,
        scoring, params, &run.proof);
    return run;
}

[[nodiscard]] ::bioseq::chaining::Anchor make_anchor(
        std::uint64_t position,
        std::uint8_t span = 15) {
    return ::bioseq::chaining::Anchor{
        .ref_position = position,
        .query_position = position,
        .is_reverse = 0,
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

}  // namespace

TEST_CASE("Block Aligner Global score/endpoints/CIGAR match scan_block.rs",
          "[block-aligner][reference-golden][global]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = ScoringMatrix<dna4_u64>::generate(1, 1, 1);

    // scan_block.rs::test_trace, NW1/Gaps{-2,-1}, block 16.
    const ReferenceRun first = run_block(
        "TTTTTTTTAAAAAAATTTTTTTTT",
        "TTAAAAAAATTTTTTTTTTTT",
        nw1, global_params());
    CHECK(first.result.score == 7);
    CHECK(first.result.max_q == 23);
    CHECK(first.result.max_t == 20);
    CHECK(packed_cigar_string(first.result.cigar) == "2M6I16M3D");
    CHECK(first.proof.accepted());
    CHECK(first.proof.gap_eligible == 1);
    CHECK(first.proof.gap_executed == 1);
    CHECK(first.proof.global_calls == 1);
    CHECK(first.proof.forward_input_calls == 1);
    CHECK(first.proof.simd_dp_vector_batches > 0);
    CHECK(first.proof.simd_dp_cells > 0);
    CHECK(first.proof.forbidden_ksw2_calls == 0);
    CHECK(first.proof.forbidden_scalar_biological_fallbacks == 0);

    // scan_block.rs::test_trace, NW1/Gaps{-2,-1}, block 32.
    const ReferenceRun second = run_block(
        "AAAAAAAAATTGCGCT", "AAAAAAAAAGCGC",
        nw1, global_params(32, 32));
    CHECK(second.result.score == 8);
    CHECK(second.result.max_q == 15);
    CHECK(second.result.max_t == 12);
    CHECK(packed_cigar_string(second.result.cigar) == "9M2I4M1I");

    // A 20-base insertion exceeds the initial 16-cell block and forces the
    // adaptive 16..=32 grow/checkpoint path. The exact affine score is
    // 32 matches - (open 2 + 19 extensions) = 11.
    const ReferenceRun adaptive = run_block(
        std::string(16, 'A') + std::string(20, 'C') + std::string(16, 'A'),
        std::string(32, 'A'),
        nw1, global_params(16, 32));
    CHECK(adaptive.result.score == 11);
    CHECK(adaptive.result.max_q == 51);
    CHECK(adaptive.result.max_t == 31);
    CHECK(packed_cigar_string(adaptive.result.cigar) == "16M20I16M");
    CHECK(adaptive.proof.adaptive_grow_events > 0);
    CHECK(adaptive.proof.accepted());

    const ReferenceRun score_only = run_block(
        "TTTTTTTTAAAAAAATTTTTTTTT",
        "TTAAAAAAATTTTTTTTTTTT",
        nw1, global_params(16, 16, false));
    CHECK(score_only.result.score == first.result.score);
    CHECK(score_only.result.max_q == first.result.max_q);
    CHECK(score_only.result.max_t == first.result.max_t);
    CHECK(score_only.result.cigar.empty());
    CHECK(score_only.proof.trace_calls == 0);
    CHECK(score_only.proof.accepted());
}

TEST_CASE("Block Global diagonal certificate is byte-identical to forced DP",
          "[block-aligner][global][diagonal-certificate][oracle]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 4, 1);
    BlockAlignerParams params = global_params(16, 32);
    params.gap_open = 4;
    params.gap_extend = 2;

    const auto compare_dna4 = [&](
            std::string_view query,
            std::string_view reference,
            bool expect_certificate) {
        const ReferenceRun optimized =
            run_block_mode<false, true, dna4_u64>(
                query, reference, scoring, params);
        const ReferenceRun forced_dp =
            run_block_mode<false, false, dna4_u64>(
                query, reference, scoring, params);
        check_align_result_identical(optimized.result, forced_dp.result);
        CHECK(optimized.proof.accepted());
        CHECK(forced_dp.proof.accepted());
        CHECK(optimized.proof.simd_diagonal_certificate_calls
              == static_cast<std::uint64_t>(expect_certificate));
        CHECK(forced_dp.proof.simd_diagonal_certificate_calls == 0);
        CHECK(forced_dp.proof.simd_dp_cells > 0);
        if (expect_certificate) {
            CHECK(optimized.proof.simd_dp_cells == 0);
            CHECK(optimized.proof.simd_dp_vector_batches == 0);
            CHECK(optimized.proof.simd_diagonal_certificate_vector_batches > 0);
            CHECK(optimized.proof.simd_diagonal_certificate_symbols
                  == query.size());
        } else {
            CHECK(optimized.proof.simd_dp_cells > 0);
            CHECK(optimized.proof.simd_diagonal_certificate_symbols == 0);
        }
    };

    const std::string exact =
        "ACGTACGTTGCATGCAAAAACCCCGGGGTTTT"
        "TGCATGCAACGTACGTCCCCAAAATTTTGGGG";
    compare_dna4(exact, exact, true);

    BlockAlignerParams score_only_params = params;
    score_only_params.trace = false;
    const ReferenceRun score_only_optimized =
        run_block_mode<false, true, dna4_u64>(
            exact, exact, scoring, score_only_params);
    const ReferenceRun score_only_forced =
        run_block_mode<false, false, dna4_u64>(
            exact, exact, scoring, score_only_params);
    check_align_result_identical(
        score_only_optimized.result, score_only_forced.result);
    CHECK(score_only_optimized.result.cigar.empty());
    CHECK(score_only_optimized.proof.simd_diagonal_certificate_calls == 1);
    CHECK(score_only_optimized.proof.simd_dp_cells == 0);

    BlockAlignerExecutionProof tampered = score_only_optimized.proof;
    tampered.simd_diagonal_certificate_vector_batches = 0;
    CHECK_FALSE(tampered.accepted());
    tampered = score_only_optimized.proof;
    tampered.simd_diagonal_certificate_symbols = 0;
    CHECK_FALSE(tampered.accepted());
    tampered = score_only_optimized.proof;
    tampered.simd_dp_cells = 1;
    CHECK_FALSE(tampered.accepted());

    std::string one_substitution = exact;
    one_substitution[17] = one_substitution[17] == 'A' ? 'C' : 'A';
    compare_dna4(exact, one_substitution, true);

    std::string two_substitutions = one_substitution;
    two_substitutions[46] = two_substitutions[46] == 'A' ? 'C' : 'A';
    compare_dna4(exact, two_substitutions, true);

    // Moving one distinct base inside a repeat resembles a compensating
    // insertion/deletion but has only two coordinate mismatches.  The strict
    // bound, not biological provenance, proves the diagonal is still unique.
    std::string repeat_query(64, 'A');
    repeat_query[31] = 'C';
    std::string repeat_shifted = repeat_query;
    repeat_shifted[31] = 'A';
    repeat_shifted[32] = 'C';
    compare_dna4(repeat_query, repeat_shifted, true);

    // Three default-scoring substitutions fall below the strict certificate
    // bound. Arbitrary seed/hash agreement cannot change that: actual bytes
    // force the old DP path.
    std::string three_substitutions = two_substitutions;
    three_substitutions[55] = three_substitutions[55] == 'A' ? 'C' : 'A';
    compare_dna4(exact, three_substitutions, false);

    // Equal-length compensating-indel geometry with many displaced bytes and
    // unequal-length single-indel geometry are both explicit fallbacks.
    const std::string displaced = "T" + exact.substr(0, exact.size() - 1);
    compare_dna4(exact, displaced, false);
    compare_dna4(exact, exact + "A", false);

    // D == U is deliberately not enough: retain old traceback tie policy.
    BlockAlignerParams tie_params = params;
    tie_params.gap_open = 1;
    tie_params.gap_extend = 1;
    const ReferenceRun tie_optimized =
        run_block_mode<false, true, dna4_u64>(
            exact, one_substitution, scoring, tie_params);
    const ReferenceRun tie_forced =
        run_block_mode<false, false, dna4_u64>(
            exact, one_substitution, scoring, tie_params);
    check_align_result_identical(tie_optimized.result, tie_forced.result);
    CHECK(tie_optimized.proof.simd_diagonal_certificate_calls == 0);
    CHECK(tie_optimized.proof.simd_dp_cells > 0);

    // A valid but non-generated score layout is outside the SIMD scan's
    // exact classification and therefore cannot be certified.
    auto unsupported_scoring = scoring;
    unsupported_scoring.at(0, 1) = 3;
    unsupported_scoring.simd_scores[1] = 3;
    REQUIRE(unsupported_scoring.block_layout_valid());
    const ReferenceRun unsupported_optimized =
        run_block_mode<false, true, dna4_u64>(
            exact, exact, unsupported_scoring, params);
    const ReferenceRun unsupported_forced =
        run_block_mode<false, false, dna4_u64>(
            exact, exact, unsupported_scoring, params);
    check_align_result_identical(
        unsupported_optimized.result, unsupported_forced.result);
    CHECK(unsupported_optimized.proof.simd_diagonal_certificate_calls == 0);
    CHECK(unsupported_optimized.proof.simd_dp_cells > 0);

    const auto dna5_scoring = ScoringMatrix<dna5_u64>::generate(2, 4, 1);
    const std::string dna5_sequence =
        "ACGTNACGTACGTACGTACGTACGTACGTACG"
        "TGCATGCATGCATGCATGCATGCATGCATGCA";
    const ReferenceRun dna5_optimized =
        run_block_mode<false, true, dna5_u64>(
            dna5_sequence, dna5_sequence, dna5_scoring, params);
    const ReferenceRun dna5_forced =
        run_block_mode<false, false, dna5_u64>(
            dna5_sequence, dna5_sequence, dna5_scoring, params);
    check_align_result_identical(dna5_optimized.result, dna5_forced.result);
    CHECK(dna5_optimized.proof.simd_diagonal_certificate_calls == 1);
    CHECK(dna5_optimized.proof.simd_dp_cells == 0);

    // Reverse-strand template coordinates are encoded to canonical reverse
    // complement before the scan. Compare those exact consumed bytes too.
    const std::string reverse_source =
        "AACCGTACGTTTGGGCCCAAATTTACGTACGT"
        "GGGGAAAACCCCTTTTACGTTGCATGCACGTA";
    const std::string reverse_target =
        reverse_complement_ascii(reverse_source);
    const ReferenceRun reverse_optimized =
        run_block_mode<true, true, dna4_u64>(
            reverse_source, reverse_target, scoring, params);
    const ReferenceRun reverse_forced =
        run_block_mode<true, false, dna4_u64>(
            reverse_source, reverse_target, scoring, params);
    check_align_result_identical(
        reverse_optimized.result, reverse_forced.result);
    CHECK(reverse_optimized.proof.simd_diagonal_certificate_calls == 1);
    CHECK(reverse_optimized.proof.simd_dp_cells == 0);
}

TEST_CASE("Block Aligner empty Global/XDrop results match scan_block.rs",
          "[block-aligner][reference-golden][empty]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = ScoringMatrix<dna4_u64>::generate(1, 1, 1);

    // scan_block.rs::test_no_x_drop uses Gaps{-11,-1}; q=10,e=1 is the
    // equivalent public convention because open=-(q+e).
    BlockAlignerParams global = global_params();
    global.gap_open = 10;
    const ReferenceRun both_empty = run_block("", "", nw1, global);
    CHECK(both_empty.result.score == 0);
    CHECK(both_empty.result.max_q == -1);
    CHECK(both_empty.result.max_t == -1);
    CHECK(both_empty.result.cigar.empty());
    CHECK(both_empty.proof.zero_work_calls == 1);
    CHECK(both_empty.proof.simd_dp_cells == 0);
    CHECK(both_empty.proof.accepted());

    const ReferenceRun empty_query = run_block("", "AAAA", nw1, global);
    CHECK(empty_query.result.score == -14);
    CHECK(empty_query.result.max_q == -1);
    CHECK(empty_query.result.max_t == 3);
    CHECK(packed_cigar_string(empty_query.result.cigar) == "4D");
    CHECK(empty_query.proof.accepted());

    const ReferenceRun empty_reference = run_block("AAAA", "", nw1, global);
    CHECK(empty_reference.result.score == -14);
    CHECK(empty_reference.result.max_q == 3);
    CHECK(empty_reference.result.max_t == -1);
    CHECK(packed_cigar_string(empty_reference.result.cigar) == "4I");

    const auto aa_ar = ScoringMatrix<dna4_u64>::generate(4, 1, 1);
    const ReferenceRun empty_xdrop = run_block(
        "AAAA", "", aa_ar,
        xdrop_params(BlockAlignerPhase::RightExtension,
                     BlockInputOrder::Forward));
    CHECK(empty_xdrop.result.score == 0);
    CHECK(empty_xdrop.result.max_q == -1);
    CHECK(empty_xdrop.result.max_t == -1);
    CHECK(empty_xdrop.result.cigar.empty());
    CHECK(empty_xdrop.proof.zero_work_calls == 1);
    CHECK(empty_xdrop.proof.accepted());
}

TEST_CASE("Block packed traceback coalesces mismatch-rich diagonals",
          "[block-aligner][reference-golden][cigar][packed-traceback][mismatch]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(4, 1, 1);
    const std::string query(64, 'A');
    std::string reference;
    reference.reserve(query.size());
    for (std::size_t i = 0; i < query.size(); ++i) {
        reference += (i & 1u) == 0 ? 'A' : 'C';
    }

    BlockAlignerParams params = global_params(64, 64);
    // Make a gap much more expensive than any diagonal choice. The exact
    // alignment therefore alternates 32 matches and 32 mismatches, while the
    // packed minimap ABI must expose one canonical M run.
    params.gap_open = 30;
    const ReferenceRun run = run_block(query, reference, scoring, params);

    CHECK(run.result.score == 96);
    CHECK(run.result.max_q == 63);
    CHECK(run.result.max_t == 63);
    CHECK(packed_cigar_string(run.result.cigar) == "64M");
    CHECK(run.result.cigar.size() == 1);
    CHECK(run.proof.forbidden_ksw2_calls == 0);
    CHECK(run.proof.forbidden_scalar_biological_fallbacks == 0);
    CHECK(run.proof.accepted());
}

TEST_CASE("Block Aligner native XDrop matches scan_block.rs and reports termination",
          "[block-aligner][reference-golden][xdrop]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    // On the A/C-only fixtures, simple DNA +4/-1 is exactly the BLOSUM62
    // A/A and A/R slice used by scan_block.rs::test_x_drop.
    const auto aa_ar = ScoringMatrix<dna4_u64>::generate(4, 1, 1);
    const BlockAlignerParams right = xdrop_params(
        BlockAlignerPhase::RightExtension, BlockInputOrder::Forward);

    const ReferenceRun complete = run_block("AAAAAA", "AAACCA", aa_ar, right);
    CHECK(complete.result.score == 14);
    CHECK(complete.result.max_q == 5);
    CHECK(complete.result.max_t == 5);
    // Packed minimap CIGAR projects the reference's Eq/X runs to M and must
    // canonicalize adjacent projected operations.
    CHECK(packed_cigar_string(complete.result.cigar) == "6M");
    CHECK_FALSE(complete.result.zdropped);
    CHECK(complete.proof.native_xdrop_terminations == 0);
    CHECK(complete.proof.simd_diagonal_certificate_calls == 0);
    CHECK(complete.proof.simd_dp_cells > 0);
    CHECK(complete.proof.accepted());

    const std::string all_a(44, 'A');
    const std::string reference = std::string(15, 'A')
        + std::string(16, 'C') + std::string(13, 'A');
    const ReferenceRun dropped = run_block(all_a, reference, aa_ar, right);
    CHECK(dropped.result.score == 60);
    CHECK(dropped.result.max_q == 14);
    CHECK(dropped.result.max_t == 14);
    CHECK(dropped.result.zdropped);
    CHECK(packed_cigar_string(dropped.result.cigar) == "15M");
    CHECK(dropped.proof.native_xdrop_calls == 1);
    CHECK(dropped.proof.native_xdrop_terminations == 1);
    CHECK(dropped.proof.right_eligible == 1);
    CHECK(dropped.proof.right_executed == 1);
    CHECK(dropped.proof.accepted());
}

TEST_CASE("Block left extension reverses compact input without scalar fallback",
          "[block-aligner][reference-golden][reverse][proof]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto aa_ar = ScoringMatrix<dna4_u64>::generate(4, 1, 1);
    const std::string query(44, 'A');

    // Reversing this source region yields the exact 15A/16R/13A research
    // fixture. The Block left phase therefore must reproduce score 60 at
    // exclusive endpoint 15 while emitting the CIGAR in source order.
    const std::string source_reference = std::string(13, 'A')
        + std::string(16, 'C') + std::string(15, 'A');
    const ReferenceRun left = run_block(
        query, source_reference, aa_ar,
        xdrop_params(BlockAlignerPhase::LeftExtension,
                     BlockInputOrder::Reversed));
    CHECK(left.result.score == 60);
    CHECK(left.result.max_q == 14);
    CHECK(left.result.max_t == 14);
    CHECK(left.result.zdropped);
    CHECK(packed_cigar_string(left.result.cigar) == "15M");
    CHECK(left.proof.left_eligible == 1);
    CHECK(left.proof.left_executed == 1);
    CHECK(left.proof.reversed_input_calls == 1);
    CHECK(left.proof.forward_input_calls == 0);
    CHECK(left.proof.reversed_compact_symbols == 88);
    CHECK(left.proof.forbidden_ksw2_calls == 0);
    CHECK(left.proof.forbidden_scalar_biological_fallbacks == 0);
    CHECK(left.proof.accepted());
}

TEST_CASE("Block reversed packed traceback preserves multi-run source order",
          "[block-aligner][reference-golden][cigar][packed-traceback][reverse][multi-run]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(4, 6, 1);

    // In working (reversed) order the unique high-scoring path is
    // 4M4I4M4D6M. The left-extension ABI must return those same operations in
    // source order, 6M4D4M4I4M, without swapping insertion/deletion meaning.
    const std::string source_query = "AAAAAAGGGGCCCCAAAA";
    const std::string source_reference = "AAAAAATTTTGGGGAAAA";
    BlockAlignerParams params = xdrop_params(
        BlockAlignerPhase::LeftExtension,
        BlockInputOrder::Reversed);
    params.gap_open = 1;
    params.min_size = 32;
    params.max_size = 32;
    params.x_drop = 100;

    const ReferenceRun run = run_block(
        source_query, source_reference, scoring, params);

    CHECK(run.result.score == 46);
    CHECK(run.result.max_q == 17);
    CHECK(run.result.max_t == 17);
    CHECK_FALSE(run.result.zdropped);
    CHECK(packed_cigar_string(run.result.cigar) == "6M4D4M4I4M");
    CHECK(run.proof.left_executed == 1);
    CHECK(run.proof.reversed_input_calls == 1);
    CHECK(run.proof.reversed_compact_symbols == 36);
    CHECK(run.proof.forbidden_ksw2_calls == 0);
    CHECK(run.proof.forbidden_scalar_biological_fallbacks == 0);
    CHECK(run.proof.accepted());
}

TEST_CASE("Block padding and admitted subregion boundaries are exact",
          "[block-aligner][reference-golden][padding][boundary]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = ScoringMatrix<dna4_u64>::generate(1, 1, 1);
    const auto aa_ar = ScoringMatrix<dna4_u64>::generate(4, 1, 1);
    constexpr std::array<std::size_t, 13> lengths{
        0, 1, 7, 8, 9, 15, 16, 17, 23, 31, 32, 33, 41};

    for (const std::size_t length : lengths) {
        INFO("admitted length=" << length);
        const std::string query_source = "C" + std::string(length, 'A') + "G";
        const std::string reference_source = "T" + std::string(length, 'A') + "C";
        const ReferenceRun global = run_block_region(
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
        CHECK(global.proof.accepted());

        const ReferenceRun reversed = run_block_region(
            query_source, reference_source,
            1, 1 + length, 1, 1 + length,
            aa_ar,
            xdrop_params(BlockAlignerPhase::LeftExtension,
                         BlockInputOrder::Reversed));
        CHECK(reversed.result.score
              == static_cast<std::int32_t>(length * 4));
        CHECK(reversed.result.max_q == static_cast<std::int32_t>(length) - 1);
        CHECK(reversed.result.max_t == static_cast<std::int32_t>(length) - 1);
        if (length == 0) {
            CHECK(reversed.result.cigar.empty());
        } else {
            CHECK(packed_cigar_string(reversed.result.cigar)
                  == std::to_string(length) + "M");
            CHECK(reversed.proof.reversed_compact_symbols == length * 2);
        }
        CHECK(reversed.proof.accepted());
    }
}

TEST_CASE("Block adaptive path grows then shrinks with upstream semantics",
          "[block-aligner][reference-golden][grow][shrink]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = ScoringMatrix<dna4_u64>::generate(1, 1, 1);
    const std::string query = std::string(32, 'A')
        + std::string(40, 'C') + std::string(256, 'A');
    const std::string reference(288, 'A');
    const ReferenceRun adaptive = run_block(
        query, reference, nw1, global_params(16, 64));

    // Verified directly against the untouched local Rust reference
    // (`scan_block.rs`, NucMatrix{1,-1}, Gaps{-2,-1}, 16..=64).  Adaptive
    // Block Aligner is heuristic and this fixture deliberately exposes its
    // documented slight loss versus full DP: it chooses two insertion runs
    // and scores 244.  This is the authoritative adaptive result, not the
    // recurrence-derived full-DP optimum.
    CHECK(adaptive.result.score == 244);
    CHECK(adaptive.result.max_q == 327);
    CHECK(adaptive.result.max_t == 287);
    CHECK(packed_cigar_string(adaptive.result.cigar)
          == "33M39I255M1I");
    CHECK(adaptive.proof.adaptive_grow_events > 0);
    CHECK(adaptive.proof.adaptive_shrink_events > 0);
    CHECK(adaptive.proof.accepted());

    // The same upstream reference with a fixed 64-cell block reaches the
    // affine optimum: 288 matches - (open 2 + 39 extensions) = 247.  Keeping
    // both goldens prevents an adaptive heuristic result from being mistaken
    // for a recurrence/checkpoint/traceback defect.
    const ReferenceRun fixed = run_block(
        query, reference, nw1, global_params(64, 64));
    CHECK(fixed.result.score == 247);
    CHECK(fixed.result.max_q == 327);
    CHECK(fixed.result.max_t == 287);
    CHECK(packed_cigar_string(fixed.result.cigar) == "32M40I256M");
    CHECK(fixed.proof.adaptive_grow_events == 0);
    CHECK(fixed.proof.adaptive_shrink_events == 0);
    CHECK(fixed.proof.accepted());
}

TEST_CASE("Block minimap projection ignores dormant KSW fields and proof is exact",
          "[block-aligner][routing][proof][no-fallback]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto nw1 = ScoringMatrix<dna4_u64>::generate(1, 1, 1);
    auto query_data = dna4_from_ascii("TTTTTTTTAAAAAAATTTTTTTTT");
    auto reference_data = dna4_from_ascii("TTAAAAAAATTTTTTTTTTTT");
    range_t query_range(query_data);
    range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_reference = bioseq::storage::make_lazy_in_memory(reference_range);

    Minimap2Params first_params;
    first_params.gap_open1 = 1;
    first_params.gap_ext1 = 1;
    first_params.block_min_size = 16;
    first_params.block_max_size = 16;
    first_params.do_cigar = true;

    Minimap2Params dormant_changed = first_params;
    dormant_changed.bandwidth = 7'777;
    dormant_changed.end_bonus = 9'999;
    dormant_changed.gap_open2 = 111;
    dormant_changed.gap_ext2 = 17;
    dormant_changed.zdrop = 23'456;  // dormant for native Global gap fill

    BlockAlignerExecutionProof first_proof;
    const KswAlignResult first = block_aligner_align_pair<false>(
        lazy_query, lazy_reference,
        0, query_data.size(), 0, reference_data.size(), nw1, first_params,
        BlockAlignerPhase::GapExtension, &first_proof);
    BlockAlignerExecutionProof changed_proof;
    const KswAlignResult changed = block_aligner_align_pair<false>(
        lazy_query, lazy_reference,
        0, query_data.size(), 0, reference_data.size(), nw1,
        dormant_changed, BlockAlignerPhase::GapExtension, &changed_proof);

    CHECK(first.score == 7);
    CHECK(changed.score == first.score);
    CHECK(changed.max_q == first.max_q);
    CHECK(changed.max_t == first.max_t);
    CHECK(packed_cigar_string(changed.cigar)
          == packed_cigar_string(first.cigar));
    CHECK(changed_proof.config_identity_xor == first_proof.config_identity_xor);
    CHECK(changed_proof.config_identity_sum == first_proof.config_identity_sum);
    CHECK(changed_proof == first_proof);
    CHECK(first_proof.accepted());

    const auto changed_scoring =
        ScoringMatrix<dna4_u64>::generate(2, 1, 1);
    BlockAlignerExecutionProof scoring_proof;
    (void)block_aligner_align_pair<false>(
        lazy_query, lazy_reference,
        0, query_data.size(), 0, reference_data.size(), changed_scoring,
        first_params, BlockAlignerPhase::GapExtension, &scoring_proof);
    CHECK(scoring_proof.config_identity_xor
          != first_proof.config_identity_xor);
    CHECK(scoring_proof.config_identity_sum
          != first_proof.config_identity_sum);
    CHECK(scoring_proof.accepted());

    auto invalid_layout = nw1;
    invalid_layout.simd_scores[0] = static_cast<std::int8_t>(
        invalid_layout.simd_scores[0] + 1);
    CHECK_FALSE(invalid_layout.block_layout_valid());

    BlockAlignerExecutionProof left_proof;
    const KswAlignResult projected_left = block_aligner_align_pair<false>(
        lazy_query, lazy_reference,
        0, query_data.size(), 0, reference_data.size(), nw1, first_params,
        BlockAlignerPhase::LeftExtension, &left_proof);
    BlockAlignerExecutionProof right_proof;
    const KswAlignResult projected_right = block_aligner_align_pair<false>(
        lazy_query, lazy_reference,
        0, query_data.size(), 0, reference_data.size(), nw1, first_params,
        BlockAlignerPhase::RightExtension, &right_proof);
    CHECK(projected_left.score >= 0);
    CHECK(projected_right.score >= 0);
    CHECK(left_proof.left_executed == 1);
    CHECK(left_proof.reversed_input_calls == 1);
    CHECK(left_proof.native_xdrop_calls == 1);
    CHECK(left_proof.accepted());
    CHECK(right_proof.right_executed == 1);
    CHECK(right_proof.forward_input_calls == 1);
    CHECK(right_proof.native_xdrop_calls == 1);
    CHECK(right_proof.accepted());

    BlockAlignerExecutionProof combined;
    combined.combine(first_proof);
    combined.combine(left_proof);
    combined.combine(right_proof);
    CHECK(combined.left_executed == 1);
    CHECK(combined.gap_executed == 1);
    CHECK(combined.right_executed == 1);
    CHECK(combined.global_calls == 1);
    CHECK(combined.native_xdrop_calls == 2);
    CHECK(combined.reversed_input_calls == 1);
    CHECK(combined.forward_input_calls == 2);
    CHECK(combined.forbidden_ksw2_calls == 0);
    CHECK(combined.forbidden_scalar_biological_fallbacks == 0);
    CHECK(combined.accepted());

    BlockAlignerExecutionProof tampered = combined;
    ++tampered.forbidden_ksw2_calls;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    ++tampered.forbidden_scalar_biological_fallbacks;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    ++tampered.left_eligible;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    tampered.routed_region_scopes = 1;
    CHECK_FALSE(tampered.accepted());
    tampered.left_route_candidates = tampered.left_executed;
    tampered.gap_route_candidates = tampered.gap_executed;
    tampered.right_route_candidates = tampered.right_executed;
    tampered.align_region_invocations = 1;
    tampered.serial_align_region_dispatches = 1;
    CHECK(tampered.accepted());
    tampered = combined;
    tampered.executed_config_identity_xor ^= 1u;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    ++tampered.executed_config_identity_sum;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    ++tampered.executed_trace_calls;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    ++tampered.executed_forward_input_calls;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    ++tampered.reversed_compact_symbols;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    --tampered.nonempty_calls_with_simd;
    CHECK_FALSE(tampered.accepted());
    tampered = combined;
    tampered.saturated = true;
    CHECK_FALSE(tampered.accepted());
}

TEST_CASE("Block production router executes left global-gap and right phases",
          "[block-aligner][routing][production-proof][full-route]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    auto query_data = dna4_from_ascii(std::string(256, 'A'));
    auto target_data = dna4_from_ascii(std::string(256, 'A'));
    range_t query_range(query_data);
    range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(1, 1, 1);

    std::vector<::bioseq::chaining::Anchor> anchors{
        make_anchor(64), make_anchor(120)};
    std::vector<AlignRegion> regions(1);
    regions[0].seed_count = 2;
    regions[0].anchor_start = 0;
    regions[0].is_reverse = false;

    Minimap2Params params;
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

    BlockAlignerExecutionProof proof;
    align_regions<BaseAlignmentAlgorithmType::BLOCK_ALIGNER>(
        std::span<AlignRegion>{regions.data(), regions.size()},
        std::span<const ::bioseq::chaining::Anchor>{
            anchors.data(), anchors.size()},
        lazy_query, lazy_target, scoring, params, &proof);

    CHECK(regions[0].has_alignment);
    CHECK_FALSE(regions[0].cigar.empty());
    CHECK(proof.align_region_invocations == 1);
    CHECK(proof.serial_align_region_dispatches == 1);
    CHECK(proof.parallel_align_region_dispatches == 0);
    CHECK(proof.routed_region_scopes == 1);
    CHECK(proof.left_route_candidates == 1);
    CHECK(proof.gap_route_candidates == 1);
    CHECK(proof.right_route_candidates == 1);
    CHECK(proof.left_executed == 1);
    CHECK(proof.gap_executed == 1);
    CHECK(proof.right_executed == 1);
    CHECK(proof.global_calls == 1);
    CHECK(proof.native_xdrop_calls == 2);
    CHECK(proof.reversed_input_calls == 1);
    CHECK(proof.forward_input_calls == 2);
    CHECK(proof.nonempty_calls_with_simd == 3);
    CHECK(proof.forbidden_ksw2_calls == 0);
    CHECK(proof.forbidden_scalar_biological_fallbacks == 0);
    CHECK(proof.accepted());
}

TEST_CASE("Block phase expectations derive independently from immutable geometry",
          "[block-aligner][routing][eligibility][tamper]") {
    std::vector<::bioseq::chaining::Anchor> anchors{
        make_anchor(64), make_anchor(120)};
    AlignRegion region;
    region.seed_count = 2;
    region.anchor_start = 0;
    Minimap2Params params;
    params.min_ksw_len = 1;

    const BlockPhaseEligibility expected = derive_block_phase_eligibility(
        region,
        std::span<const ::bioseq::chaining::Anchor>{
            anchors.data(), anchors.size()},
        0,
        50, 50,
        121, 121,
        0, 0,
        180, 180,
        params);
    CHECK(expected.left == 1);
    CHECK(expected.gap == 1);
    CHECK(expected.right == 1);

    BlockAlignerExecutionProof proof;
    proof.align_region_invocations = 1;
    proof.serial_align_region_dispatches = 1;
    proof.routed_region_scopes = 1;
    proof.left_route_candidates = expected.left;
    proof.gap_route_candidates = expected.gap;
    proof.right_route_candidates = expected.right;
    // Expectations alone cannot manufacture execution evidence.
    CHECK_FALSE(proof.accepted());
}

TEST_CASE("Block production proof reduction is deterministic in parallel",
          "[block-aligner][routing][parallel][proof]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    auto query_data = dna4_from_ascii(std::string(512, 'A'));
    auto target_data = dna4_from_ascii(std::string(512, 'A'));
    range_t query_range(query_data);
    range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(1, 1, 1);

    std::vector<::bioseq::chaining::Anchor> anchors;
    std::vector<AlignRegion> fixture_regions(4);
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

    Minimap2Params params;
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

    auto run = [&](bioseq::parallelism::PipelineConcurrencyMode mode) {
        auto& pool = bioseq::parallelism::GlobalPool::instance();
        pool.restart(bioseq::parallelism::PoolConfig{
            .max_concurrency = mode
                    == bioseq::parallelism::PipelineConcurrencyMode::SINGLE
                ? 1u : 2u,
            .mode = mode,
        });
        std::vector<AlignRegion> regions = fixture_regions;
        BlockAlignerExecutionProof proof;
        align_regions<BaseAlignmentAlgorithmType::BLOCK_ALIGNER>(
            std::span<AlignRegion>{regions.data(), regions.size()},
            std::span<const ::bioseq::chaining::Anchor>{
                anchors.data(), anchors.size()},
            lazy_query, lazy_target, scoring, params, &proof);
        return std::pair{std::move(regions), proof};
    };

    auto [serial_regions, serial_proof] = run(
        bioseq::parallelism::PipelineConcurrencyMode::SINGLE);
    auto [parallel_regions, parallel_proof] = run(
        bioseq::parallelism::PipelineConcurrencyMode::TASK_PARALLEL);
    bioseq::parallelism::GlobalPool::instance().shutdown();

    CHECK(serial_proof.align_region_invocations == 1);
    CHECK(serial_proof.serial_align_region_dispatches == 1);
    CHECK(serial_proof.parallel_align_region_dispatches == 0);
    CHECK(parallel_proof.align_region_invocations == 1);
    CHECK(parallel_proof.serial_align_region_dispatches == 0);
    CHECK(parallel_proof.parallel_align_region_dispatches == 1);
    CHECK(serial_proof.accepted());
    CHECK(parallel_proof.accepted());

    serial_proof.align_region_invocations = 0;
    serial_proof.serial_align_region_dispatches = 0;
    parallel_proof.align_region_invocations = 0;
    parallel_proof.parallel_align_region_dispatches = 0;
    CHECK(serial_proof == parallel_proof);
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
          "[block-aligner][reference-golden][wide-alphabet][two-lut]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    using aa27_u64 = sequence_custom_alphabet<seq::aa27, std::uint64_t>;
    using aa27_range_t = compact_sequence_range<aa27_u64>;

    std::vector<seq::aa27> query_data(54);
    std::vector<seq::aa27> target_data(54);
    for (std::size_t index = 0; index < query_data.size(); ++index) {
        seq::aa27 letter;
        letter.assign_rank(static_cast<decltype(letter.to_rank())>(index % 27));
        query_data[index] = letter;
        target_data[index] = letter;
    }
    aa27_range_t query_range(query_data);
    aa27_range_t target_range(target_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto scoring = ScoringMatrix<aa27_u64>::generate(2, 1, 1);
    BlockAlignerExecutionProof proof;
    const KswAlignResult result = HwyBlockAligner::align<false, false>(
        lazy_query, lazy_target,
        0, query_data.size(), 0, target_data.size(),
        scoring, global_params(16, 32), &proof);

    CHECK(result.score == 108);
    CHECK(result.max_q == 53);
    CHECK(result.max_t == 53);
    CHECK(packed_cigar_string(result.cigar) == "54M");
    CHECK(proof.nonempty_calls_with_simd == 1);
    CHECK(proof.forbidden_scalar_biological_fallbacks == 0);
    CHECK(proof.accepted());
}

TEST_CASE("Block native L16 path matches the cross-lane golden",
          "[block-aligner][reference-golden][lane16]") {
    const std::size_t lanes = hn::Lanes(BlockAlignerSimdTag{});
    if (lanes != 16) {
        SKIP("native host exposes L=8; L=16 requires an AVX2 local run");
    }

    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 1, 1);
    const std::string sequence =
        "ACGTACGTACGTACGTTGCATGCATGCATGCA"
        "AAAACCCCGGGGTTTTACGTACGTACGTACGT";
    const ReferenceRun run = run_block_mode<false, false, dna4_u64>(
        sequence, sequence, scoring, global_params(16, 32));
    CHECK(run.result.score == 128);
    CHECK(run.result.max_q == 63);
    CHECK(run.result.max_t == 63);
    CHECK(packed_cigar_string(run.result.cigar) == "64M");
    CHECK(run.proof.simd_lane8_calls == 0);
    CHECK(run.proof.simd_lane16_calls == 1);
    CHECK(run.proof.simd_dp_vector_batches > 0);
    CHECK(run.proof.accepted());
}

TEST_CASE("Block native contract rejects unsupported geometry/mode claims",
          "[block-aligner][contract]") {
    const std::size_t lanes = hn::Lanes(BlockAlignerSimdTag{});
    CHECK(block_aligner_lane_shape_supported(8));
    CHECK(block_aligner_lane_shape_supported(16));
    CHECK_FALSE(block_aligner_lane_shape_supported(4));
    CHECK_FALSE(block_aligner_lane_shape_supported(32));

    BlockAlignerParams valid = global_params();
    CHECK(validate_block_aligner_contract(valid, lanes)
          == BlockAlignerContractError::None);

    BlockAlignerParams bad_gap = valid;
    bad_gap.gap_open = 0;
    CHECK(validate_block_aligner_contract(bad_gap, lanes)
          == BlockAlignerContractError::GapPenalty);

    BlockAlignerParams bad_geometry = valid;
    bad_geometry.min_size = 24;
    bad_geometry.max_size = 32;
    CHECK(validate_block_aligner_contract(bad_geometry, lanes)
          == BlockAlignerContractError::BlockGeometry);

    BlockAlignerParams ksw_like_zdrop_claim = valid;
    ksw_like_zdrop_claim.x_drop = 400;
    CHECK(validate_block_aligner_contract(ksw_like_zdrop_claim, lanes)
          == BlockAlignerContractError::XDropThreshold);

    BlockAlignerParams wrong_phase_mode = valid;
    wrong_phase_mode.phase = BlockAlignerPhase::LeftExtension;
    CHECK(validate_block_aligner_contract(wrong_phase_mode, lanes)
          == BlockAlignerContractError::PhaseMode);

    BlockAlignerExecutionProof no_calls;
    CHECK(no_calls.accepted());
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
