// Golden differential tests for the Highway Block Aligner port.
//
// The expected values below are copied from the local upstream research tree:
//   block-aligner-research/src/scan_block.rs::tests
// They intentionally compare the native single-affine Global/XDrop contract,
// not KSW2 z-drop or dual-affine behaviour.

#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences.minimap_port;
import bioseq.sequences.chaining.utils;
import bioseq.common.context;
import bioseq.storage.lazy;
import bioseq.storage.scanner;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;

namespace bioseq::simd {

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = compact_sequence_range<dna4_u64>;
using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;

template<sequence_alphabet Alphabet>
[[nodiscard]] const AlignmentKernels<Alphabet>& fixed_alignment_kernels() {
    const auto& root =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity = root.complete()
        ? root.template resolve_alphabet_identity<Alphabet>() : nullptr;
    if (identity == nullptr
        || root.resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated fixed-target test table lacks alphabet identity"};
    }
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code
            && entry.typed_table != nullptr) {
            return *static_cast<const AlignmentKernels<Alphabet>*>(
                entry.typed_table);
        }
    }
    throw std::logic_error{
        "generated fixed-target test table lacks alignment alphabet"};
}

struct ReferenceRun {
    KswAlignResult result;
    BlockAlignerExecutionProof proof;
    BlockAlignerConstructionEvidence construction;
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
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto reference_view =
        bioseq::storage::as_lazy_sequence_view(lazy_reference);

    ReferenceRun run;
    run.result = fixed_alignment_kernels<seq::dna4>().block_align(
        false, true, query_view, reference_view,
        0, query.size(), 0, reference.size(), scoring, params, &run.proof,
        &run.construction);
    return run;
}

template<bool strand_reversed, bool enable_diagonal_certificate, class Sequence>
[[nodiscard]] ReferenceRun run_block_region_mode(
        std::string_view query_source,
        std::string_view reference_source,
        std::size_t query_start,
        std::size_t query_end,
        std::size_t reference_start,
        std::size_t reference_end,
        const ScoringMatrix<Sequence>& scoring,
        const BlockAlignerParams& params) {
    using typed_range_t = compact_sequence_range<Sequence>;
    using alphabet_t = typename typed_range_t::traits::alphabet_type;
    std::vector<alphabet_t> query_data(query_source.size());
    std::vector<alphabet_t> reference_data(reference_source.size());
    for (std::size_t i = 0; i < query_source.size(); ++i) {
        query_data[i].assign_char(query_source[i]);
    }
    for (std::size_t i = 0; i < reference_source.size(); ++i) {
        reference_data[i].assign_char(reference_source[i]);
    }
    typed_range_t query_range(query_data);
    typed_range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_reference = bioseq::storage::make_lazy_in_memory(reference_range);
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto reference_view =
        bioseq::storage::as_lazy_sequence_view(lazy_reference);

    ReferenceRun run;
    run.result = fixed_alignment_kernels<alphabet_t>().block_align(
        strand_reversed, enable_diagonal_certificate,
        query_view, reference_view,
        query_start, query_end, reference_start, reference_end,
        scoring, params, &run.proof, &run.construction);
    return run;
}

template<bool strand_reversed, bool enable_diagonal_certificate, class Sequence>
[[nodiscard]] ReferenceRun run_block_mode(
        std::string_view query,
        std::string_view reference,
        const ScoringMatrix<Sequence>& scoring,
        const BlockAlignerParams& params) {
    return run_block_region_mode<
        strand_reversed, enable_diagonal_certificate, Sequence>(
            query, reference, 0, query.size(), 0, reference.size(),
            scoring, params);
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
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto reference_view =
        bioseq::storage::as_lazy_sequence_view(lazy_reference);

    ReferenceRun run;
    run.result = fixed_alignment_kernels<seq::dna4>().block_align(
        false, true, query_view, reference_view,
        query_start, query_end, reference_start, reference_end,
        scoring, params, &run.proof, &run.construction);
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

[[nodiscard]] ::bioseq::chaining::Anchor make_anchor_at(
        std::uint64_t reference_position,
        std::uint64_t query_position,
        std::uint8_t span = 1) {
    return ::bioseq::chaining::Anchor{
        .ref_position = reference_position,
        .query_position = query_position,
        .is_reverse = 0,
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

void check_region_alignment_identical(
        const AlignRegion& optimized,
        const AlignRegion& forced_dp) {
    CHECK(optimized.query_start == forced_dp.query_start);
    CHECK(optimized.query_end == forced_dp.query_end);
    CHECK(optimized.ref_start == forced_dp.ref_start);
    CHECK(optimized.ref_end == forced_dp.ref_end);
    CHECK(optimized.dp_score == forced_dp.dp_score);
    CHECK(optimized.dp_max_score == forced_dp.dp_max_score);
    CHECK(optimized.has_alignment == forced_dp.has_alignment);
    CHECK(optimized.cigar.size() == forced_dp.cigar.size());
    CHECK(format_cigar(optimized.cigar) == format_cigar(forced_dp.cigar));
}

struct ScoringMatrixGenerationCase {
    std::int8_t match;
    std::int8_t mismatch;
    std::int8_t ambiguity;
    std::string_view description;
};

constexpr std::array<ScoringMatrixGenerationCase, 6>
    scoring_matrix_generation_cases{{
        {4, 6, 1, "positive penalty magnitudes"},
        {-4, -6, -1, "fully signed scores"},
        {7, -3, -2, "positive match and signed penalties"},
        {-7, 3, 2, "signed match and positive penalties"},
        {0, 0, 0, "zero scores"},
        {127, 126, 125, "positive int8 limits"},
    }};

template<class Alphabet>
void check_scoring_matrix_facade_parity(std::string_view alphabet_name) {
    using legacy_source =
        sequence_custom_alphabet<Alphabet, std::uint64_t>;
    using canonical_matrix = ScoringMatrix<Alphabet>;
    using legacy_matrix = ScoringMatrix<legacy_source>;

    static_assert(std::is_same_v<canonical_matrix, legacy_matrix>);
    static_assert(std::is_same_v<
        typename canonical_matrix::alphabet_type, Alphabet>);
    static_assert(canonical_matrix::alphabet_dim
                  == legacy_matrix::alphabet_dim);
    static_assert(canonical_matrix::SIMD_ROWS
                  == legacy_matrix::SIMD_ROWS);
    static_assert(canonical_matrix::SIMD_COLS
                  == legacy_matrix::SIMD_COLS);
    static_assert(canonical_matrix::NULL_BYTE
                  == legacy_matrix::NULL_BYTE);

    constexpr auto sentinel = std::numeric_limits<std::int8_t>::min();
    constexpr std::size_t dim = canonical_matrix::alphabet_dim;
    static_assert(dim > 1u);
    static_assert(canonical_matrix::SIMD_ROWS == dim + 1u);
    static_assert(canonical_matrix::SIMD_COLS >= dim + 1u);
    static_assert(canonical_matrix::NULL_BYTE == dim);

    for (const auto& generation : scoring_matrix_generation_cases) {
        const auto canonical = canonical_matrix::generate(
            generation.match, generation.mismatch, generation.ambiguity);
        const auto legacy = legacy_matrix::generate(
            generation.match, generation.mismatch, generation.ambiguity);

        INFO("alphabet=" << alphabet_name
             << " score-case=" << generation.description
             << " scores=(" << static_cast<int>(generation.match)
             << ',' << static_cast<int>(generation.mismatch)
             << ',' << static_cast<int>(generation.ambiguity) << ')');
        REQUIRE(canonical.block_layout_valid());
        REQUIRE(legacy.block_layout_valid());
        CHECK(canonical.block_semantic_identity()
              == legacy.block_semantic_identity());
        CHECK(canonical.match_score() == legacy.match_score());
        CHECK(canonical.mismatch_score() == legacy.mismatch_score());
        CHECK(canonical.wildcard_score() == legacy.wildcard_score());

        const auto expected_match = generation.match > 0
            ? generation.match
            : static_cast<std::int8_t>(-generation.match);
        const auto expected_mismatch = generation.mismatch > 0
            ? static_cast<std::int8_t>(-generation.mismatch)
            : generation.mismatch;
        const auto expected_ambiguity = generation.ambiguity > 0
            ? static_cast<std::int8_t>(-generation.ambiguity)
            : generation.ambiguity;

        for (std::size_t row = 0; row < dim; ++row) {
            for (std::size_t col = 0; col < dim; ++col) {
                INFO("scalar-cell row=" << row << " col=" << col);
                const auto expected = row == dim - 1u || col == dim - 1u
                    ? expected_ambiguity
                    : row == col ? expected_match : expected_mismatch;
                CHECK(canonical.at(static_cast<int>(row),
                                   static_cast<int>(col)) == expected);
                CHECK(legacy.at(static_cast<int>(row),
                                static_cast<int>(col)) == expected);
                CHECK(legacy.at(static_cast<int>(row),
                                static_cast<int>(col))
                      == canonical.at(static_cast<int>(row),
                                      static_cast<int>(col)));
            }
        }

        for (std::size_t row = 0;
             row < canonical_matrix::SIMD_ROWS; ++row) {
            for (std::size_t col = 0;
                 col < canonical_matrix::SIMD_COLS; ++col) {
                INFO("simd-cell row=" << row << " col=" << col);
                const auto expected = row < dim && col < dim
                    ? canonical.at(static_cast<int>(row),
                                   static_cast<int>(col))
                    : sentinel;
                CHECK(canonical.simd_row_ptr(row)[col] == expected);
                CHECK(legacy.simd_row_ptr(row)[col] == expected);
                CHECK(legacy.simd_row_ptr(row)[col]
                      == canonical.simd_row_ptr(row)[col]);
            }
        }
    }
}

}  // namespace

TEST_CASE("ScoringMatrix boundary adapters exhaustively match the canonical facade",
          "[block-aligner][scoring-matrix][facade][doc0377]") {
    check_scoring_matrix_facade_parity<seq::dna4>("dna4");
    check_scoring_matrix_facade_parity<seq::dna5>("dna5");
    check_scoring_matrix_facade_parity<seq::aa20>("aa20");
    check_scoring_matrix_facade_parity<seq::aa27>("aa27");
}

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
        CHECK(forced_dp.construction.align_arena_scopes == 1);
        CHECK(forced_dp.construction.padded_sequence_allocations == 2);
        CHECK(forced_dp.construction.scratch_allocations == 1);
        CHECK(forced_dp.construction.trace_allocations == 1);
        if (expect_certificate) {
            CHECK(optimized.proof.simd_dp_cells == 0);
            CHECK(optimized.proof.simd_dp_vector_batches == 0);
            CHECK(optimized.proof.simd_diagonal_certificate_vector_batches > 0);
            CHECK(optimized.proof.simd_diagonal_certificate_symbols
                  == query.size());
            CHECK(optimized.construction.align_arena_scopes == 0);
            CHECK(optimized.construction.padded_sequence_allocations == 0);
            CHECK(optimized.construction.scratch_allocations == 0);
            CHECK(optimized.construction.trace_allocations == 0);
        } else {
            CHECK(optimized.proof.simd_dp_cells > 0);
            CHECK(optimized.proof.simd_diagonal_certificate_symbols == 0);
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

    // Cross the fixed 1024-symbol scan boundary and leave one active byte in
    // the final hardware vector for every admitted lane shape. This is the
    // NEON full-load tail safety oracle.
    std::string long_unaligned_exact(1057, 'A');
    constexpr std::string_view dna4_cycle = "ACGT";
    for (std::size_t i = 0; i < long_unaligned_exact.size(); ++i) {
        long_unaligned_exact[i] = dna4_cycle[i % dna4_cycle.size()];
    }
    compare_dna4(long_unaligned_exact, long_unaligned_exact, true);

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
    CHECK(score_only_optimized.construction.align_arena_scopes == 0);
    CHECK(score_only_optimized.construction.padded_sequence_allocations == 0);
    CHECK(score_only_optimized.construction.scratch_allocations == 0);
    CHECK(score_only_optimized.construction.trace_allocations == 0);

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
    CHECK(dna5_optimized.construction.align_arena_scopes == 0);
    CHECK(dna5_optimized.construction.padded_sequence_allocations == 0);
    CHECK(dna5_optimized.construction.scratch_allocations == 0);

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
    CHECK(reverse_optimized.construction.align_arena_scopes == 0);
    CHECK(reverse_optimized.construction.padded_sequence_allocations == 0);
    CHECK(reverse_optimized.construction.scratch_allocations == 0);

    // Nonzero reverse-complement coordinates must remain continuous when the
    // early scan splits the requested subregion at its 1024-symbol boundary.
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
    const ReferenceRun reverse_subregion_optimized =
        run_block_region_mode<true, true, dna4_u64>(
            reverse_subregion_source, reverse_subregion_target,
            reverse_query_start,
            reverse_query_start + reverse_region_size,
            reverse_reference_start,
            reverse_reference_start + reverse_region_size,
            scoring, params);
    const ReferenceRun reverse_subregion_forced =
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
    CHECK(reverse_subregion_optimized.proof.accepted());
    CHECK(reverse_subregion_forced.proof.accepted());
    CHECK(reverse_subregion_optimized.proof.simd_diagonal_certificate_calls
          == 1);
    CHECK(reverse_subregion_optimized.proof.simd_diagonal_certificate_symbols
          == reverse_region_size);
    CHECK(reverse_subregion_optimized.proof.simd_dp_cells == 0);
    CHECK(reverse_subregion_forced.proof.simd_dp_cells > 0);
    CHECK(reverse_subregion_optimized.construction.align_arena_scopes == 0);
    CHECK(reverse_subregion_optimized.construction
              .padded_sequence_allocations == 0);
    CHECK(reverse_subregion_optimized.construction.scratch_allocations == 0);
    CHECK(reverse_subregion_optimized.construction.trace_allocations == 0);
}

TEST_CASE("Packed DNA4 gap certificate handles unaligned forward and reverse windows",
          "[block-aligner][global][packed-certificate][oracle]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 4, 1);
    BlockAlignerParams params = global_params(16, 64);
    params.gap_open = 4;
    params.gap_extend = 2;
    const BlockGlobalGapCertificatePlan plan =
        fixed_alignment_kernels<seq::dna4>().block_prepare(scoring, params);
    REQUIRE(plan.contract_supported);
    REQUIRE(plan.generated_matrix_supported);

    constexpr std::string_view cycle = "ACGT";
    const auto patterned = [&](std::size_t length, std::size_t phase) {
        std::string result(length, 'A');
        for (std::size_t i = 0; i < length; ++i) {
            result[i] = cycle[(i * 3u + i / 7u + phase) % cycle.size()];
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
        auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
        auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
        const auto query_view =
            bioseq::storage::as_lazy_sequence_view(lazy_query);
        const auto target_view =
            bioseq::storage::as_lazy_sequence_view(lazy_target);
        const BlockPackedCertificateAttempt attempt =
            fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
                false, query_view, target_view,
                query_start, query_start + length,
                target_start, target_start + length, plan);
        REQUIRE(attempt.decision
                == BlockPackedCertificateDecision::Certified);
        CHECK(attempt.certificate.score
              == static_cast<std::int32_t>(length * 2u));
        CHECK(attempt.certificate.symbols == length);
        CHECK(attempt.certificate.vector_batches > 0u);

        const ReferenceRun forced_dp =
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

    // Nonzero reverse logical coordinates cross both packed-word and 1024
    // boundaries.  The target is the exact canonical RC window consumed by
    // the old aligner.
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
        bioseq::storage::make_lazy_in_memory(reverse_query_range);
    auto lazy_reverse_target =
        bioseq::storage::make_lazy_in_memory(reverse_target_range);
    const auto reverse_query_view =
        bioseq::storage::as_lazy_sequence_view(lazy_reverse_query);
    const auto reverse_target_view =
        bioseq::storage::as_lazy_sequence_view(lazy_reverse_target);
    const BlockPackedCertificateAttempt reverse_attempt =
        fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
            true, reverse_query_view, reverse_target_view,
            reverse_query_start, reverse_query_start + reverse_length,
            reverse_target_start, reverse_target_start + reverse_length,
            plan);
    REQUIRE(reverse_attempt.decision
            == BlockPackedCertificateDecision::Certified);
    BlockAlignerExecutionProof forward_certificate_proof;
    record_block_certified_global_gap(
        plan, reverse_attempt.certificate, false,
        &forward_certificate_proof);
    BlockAlignerExecutionProof reverse_certificate_proof;
    record_block_certified_global_gap(
        plan, reverse_attempt.certificate, true,
        &reverse_certificate_proof);
    CHECK(forward_certificate_proof.accepted());
    CHECK(reverse_certificate_proof.accepted());
    CHECK(forward_certificate_proof.config_identity_xor
          != reverse_certificate_proof.config_identity_xor);
    CHECK(forward_certificate_proof.config_identity_sum
          != reverse_certificate_proof.config_identity_sum);
    const ReferenceRun reverse_forced =
        run_block_region_mode<true, false, dna4_u64>(
            reverse_query_source, reverse_target_source,
            reverse_query_start, reverse_query_start + reverse_length,
            reverse_target_start, reverse_target_start + reverse_length,
            scoring, params);
    CHECK(reverse_attempt.certificate.score == reverse_forced.result.score);
    CHECK(packed_cigar_string(reverse_forced.result.cigar)
          == std::to_string(reverse_length) + "M");

    // The packed path scores substitutions exactly.  One substitution still
    // proves the diagonal under q=4/e=2; three do not.  Under q=1/e=1 the
    // one-substitution score equals the alternative bound and must reject to
    // preserve the old traceback tie policy.
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
        bioseq::storage::make_lazy_in_memory(decision_query_range);
    auto lazy_decision_target =
        bioseq::storage::make_lazy_in_memory(decision_target_range);
    const auto decision_query_view =
        bioseq::storage::as_lazy_sequence_view(lazy_decision_query);
    const auto decision_target_view =
        bioseq::storage::as_lazy_sequence_view(lazy_decision_target);
    const auto one_substitution =
        fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
            false, decision_query_view, decision_target_view,
            decision_query_start, decision_query_start + decision_length,
            decision_target_start, decision_target_start + decision_length,
            plan);
    CHECK(one_substitution.decision
          == BlockPackedCertificateDecision::Certified);
    const ReferenceRun one_substitution_forced =
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
        bioseq::storage::make_lazy_in_memory(rejected_target_range);
    const auto rejected_target_view =
        bioseq::storage::as_lazy_sequence_view(lazy_rejected_target);
    const auto rejected =
        fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
            false, decision_query_view, rejected_target_view,
            decision_query_start, decision_query_start + decision_length,
            decision_target_start, decision_target_start + decision_length,
            plan);
    CHECK(rejected.decision == BlockPackedCertificateDecision::Rejected);

    BlockAlignerParams tie_params = params;
    tie_params.gap_open = 1;
    tie_params.gap_extend = 1;
    const auto tie_plan = fixed_alignment_kernels<seq::dna4>().block_prepare(
        scoring, tie_params);
    const auto tie =
        fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
            false, decision_query_view, decision_target_view,
            decision_query_start, decision_query_start + decision_length,
            decision_target_start, decision_target_start + decision_length,
            tie_plan);
    CHECK(tie.decision == BlockPackedCertificateDecision::Rejected);
}

TEST_CASE("Packed gap executor matches forced-old mixed production region",
          "[block-aligner][routing][packed-certificate][oracle]") {
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
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::as_lazy_sequence_view(lazy_target);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 4, 1);
    std::vector<::bioseq::chaining::Anchor> anchors{
        make_anchor_at(0u, 0u),
        make_anchor_at(32u, 32u),
        make_anchor_at(64u, 64u),
        make_anchor_at(97u, 97u),
        make_anchor_at(131u, 130u),
    };
    AlignRegion fixture;
    fixture.seed_count = static_cast<std::int32_t>(anchors.size());
    fixture.anchor_start = 0;
    fixture.is_reverse = false;

    Minimap2Params params;
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

    AlignRegion optimized = fixture;
    BlockAlignerExecutionProof optimized_proof;
    align_regions<BaseAlignmentAlgorithmType::BLOCK_ALIGNER, true>(
        std::span<AlignRegion>{&optimized, 1u},
        std::span<const ::bioseq::chaining::Anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params, &optimized_proof);
    AlignRegion forced_dp = fixture;
    BlockAlignerExecutionProof forced_proof;
    align_regions<BaseAlignmentAlgorithmType::BLOCK_ALIGNER, false>(
        std::span<AlignRegion>{&forced_dp, 1u},
        std::span<const ::bioseq::chaining::Anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params, &forced_proof);
    check_region_alignment_identical(optimized, forced_dp);
    CHECK(optimized_proof.accepted());
    CHECK(forced_proof.accepted());
    CHECK(optimized_proof.gap_executed == 4u);
    CHECK(optimized_proof.simd_diagonal_certificate_calls == 2u);
    CHECK(optimized_proof.simd_dp_cells > 0u);
    CHECK(forced_proof.gap_executed == 4u);
    CHECK(forced_proof.simd_diagonal_certificate_calls == 0u);
    CHECK(forced_proof.simd_dp_cells > optimized_proof.simd_dp_cells);

    params.do_cigar = false;
    AlignRegion score_only_optimized = fixture;
    BlockAlignerExecutionProof score_only_optimized_proof;
    align_regions<BaseAlignmentAlgorithmType::BLOCK_ALIGNER, true>(
        std::span<AlignRegion>{&score_only_optimized, 1u},
        std::span<const ::bioseq::chaining::Anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params,
        &score_only_optimized_proof);
    AlignRegion score_only_forced = fixture;
    BlockAlignerExecutionProof score_only_forced_proof;
    align_regions<BaseAlignmentAlgorithmType::BLOCK_ALIGNER, false>(
        std::span<AlignRegion>{&score_only_forced, 1u},
        std::span<const ::bioseq::chaining::Anchor>{
            anchors.data(), anchors.size()},
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params,
        &score_only_forced_proof);
    check_region_alignment_identical(score_only_optimized, score_only_forced);
    CHECK(score_only_optimized.cigar.empty());
    CHECK(score_only_optimized_proof.simd_diagonal_certificate_calls == 2u);
    CHECK(score_only_optimized_proof.accepted());
    CHECK(score_only_forced_proof.accepted());
}

TEST_CASE("Packed gap certificate preserves fragmented DNA5 and matrix fallbacks",
          "[block-aligner][global][packed-certificate][fallback]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 4, 1);
    BlockAlignerParams params = global_params(16, 64);
    params.gap_open = 4;
    params.gap_extend = 2;
    const auto plan = fixed_alignment_kernels<seq::dna4>().block_prepare(
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

    using chunk1 = bioseq::storage::sequence_scanner_test_chunked<
        std::uint64_t, 1>;
    bioseq::storage::lazy_compact_sequence<dna4_u64, chunk1> fragmented_query{
        chunk1::from_range(query_range)};
    bioseq::storage::lazy_compact_sequence<dna4_u64, chunk1> fragmented_target{
        chunk1::from_range(target_range)};
    const auto fragmented_query_view =
        bioseq::storage::as_lazy_sequence_view(fragmented_query);
    const auto fragmented_target_view =
        bioseq::storage::as_lazy_sequence_view(fragmented_target);
    const auto fragmented_attempt =
        fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
            false, fragmented_query_view, fragmented_target_view,
            0u, sequence.size(), 0u, sequence.size(), plan);
    CHECK(fragmented_attempt.decision
          == BlockPackedCertificateDecision::NotEligible);
    BlockAlignerExecutionProof fragmented_current_proof;
    const KswAlignResult fragmented_current =
        fixed_alignment_kernels<seq::dna4>().block_align(
            false, true, fragmented_query_view, fragmented_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &fragmented_current_proof, nullptr);
    BlockAlignerExecutionProof fragmented_forced_proof;
    const KswAlignResult fragmented_forced =
        fixed_alignment_kernels<seq::dna4>().block_align(
            false, false, fragmented_query_view, fragmented_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &fragmented_forced_proof, nullptr);
    check_align_result_identical(fragmented_current, fragmented_forced);
    CHECK(fragmented_current_proof.simd_diagonal_certificate_calls == 1u);
    CHECK(fragmented_forced_proof.simd_diagonal_certificate_calls == 0u);
    CHECK(fragmented_current_proof.accepted());
    CHECK(fragmented_forced_proof.accepted());

    // Reverse fragmented coordinates use the current byte encoder too.
    const std::string reverse_target_text =
        reverse_complement_ascii(sequence);
    auto reverse_target_data = dna4_from_ascii(reverse_target_text);
    range_t reverse_target_range(reverse_target_data);
    using chunk32 = bioseq::storage::sequence_scanner_test_chunked<
        std::uint64_t, 32>;
    bioseq::storage::lazy_compact_sequence<dna4_u64, chunk32> reverse_query{
        chunk32::from_range(query_range)};
    bioseq::storage::lazy_compact_sequence<dna4_u64, chunk32> reverse_target{
        chunk32::from_range(reverse_target_range)};
    const auto reverse_query_view =
        bioseq::storage::as_lazy_sequence_view(reverse_query);
    const auto reverse_target_view =
        bioseq::storage::as_lazy_sequence_view(reverse_target);
    const auto reverse_fragmented_attempt =
        fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
            true, reverse_query_view, reverse_target_view,
            0u, sequence.size(), 0u, sequence.size(), plan);
    CHECK(reverse_fragmented_attempt.decision
          == BlockPackedCertificateDecision::NotEligible);
    BlockAlignerExecutionProof reverse_current_proof;
    const KswAlignResult reverse_current =
        fixed_alignment_kernels<seq::dna4>().block_align(
            true, true, reverse_query_view, reverse_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &reverse_current_proof, nullptr);
    BlockAlignerExecutionProof reverse_forced_proof;
    const KswAlignResult reverse_forced =
        fixed_alignment_kernels<seq::dna4>().block_align(
            true, false, reverse_query_view, reverse_target_view,
            0u, sequence.size(), 0u, sequence.size(), scoring, params,
            &reverse_forced_proof, nullptr);
    check_align_result_identical(reverse_current, reverse_forced);
    CHECK(reverse_current_proof.simd_diagonal_certificate_calls == 1u);
    CHECK(reverse_forced_proof.simd_diagonal_certificate_calls == 0u);

    const auto dna5_scoring = ScoringMatrix<dna5_u64>::generate(2, 4, 1);
    const std::string dna5_text = "ACGTNACGTACGTACGTACGTACGTACGTACG";
    std::vector<seq::dna5> dna5_data(dna5_text.size());
    for (std::size_t i = 0; i < dna5_text.size(); ++i) {
        dna5_data[i].assign_char(dna5_text[i]);
    }
    using dna5_range_t = compact_sequence_range<dna5_u64>;
    dna5_range_t dna5_query_range(dna5_data);
    dna5_range_t dna5_target_range(dna5_data);
    auto lazy_dna5_query =
        bioseq::storage::make_lazy_in_memory(dna5_query_range);
    auto lazy_dna5_target =
        bioseq::storage::make_lazy_in_memory(dna5_target_range);
    const auto dna5_query_view =
        bioseq::storage::as_lazy_sequence_view(lazy_dna5_query);
    const auto dna5_target_view =
        bioseq::storage::as_lazy_sequence_view(lazy_dna5_target);
    const auto dna5_plan = fixed_alignment_kernels<seq::dna5>().block_prepare(
        dna5_scoring, params);
    const auto dna5_attempt =
        fixed_alignment_kernels<seq::dna5>().block_try_certified_gap(
            false, dna5_query_view, dna5_target_view,
            0u, dna5_text.size(), 0u, dna5_text.size(), dna5_plan);
    CHECK(dna5_attempt.decision
          == BlockPackedCertificateDecision::NotEligible);

    auto unsupported_scoring = scoring;
    unsupported_scoring.at(0, 1) = 3;
    unsupported_scoring.simd_scores[1] = 3;
    REQUIRE(unsupported_scoring.block_layout_valid());
    const auto unsupported_plan =
        fixed_alignment_kernels<seq::dna4>().block_prepare(
            unsupported_scoring, params);
    CHECK_FALSE(unsupported_plan.generated_matrix_supported);
    auto resident_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto resident_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto resident_query_view =
        bioseq::storage::as_lazy_sequence_view(resident_query);
    const auto resident_target_view =
        bioseq::storage::as_lazy_sequence_view(resident_target);
    const auto unsupported_attempt =
        fixed_alignment_kernels<seq::dna4>().block_try_certified_gap(
            false, resident_query_view, resident_target_view,
            0u, sequence.size(), 0u, sequence.size(), unsupported_plan);
    CHECK(unsupported_attempt.decision
          == BlockPackedCertificateDecision::NotEligible);
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
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto reference_view =
        bioseq::storage::as_lazy_sequence_view(lazy_reference);

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
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), first_params,
        BlockAlignerPhase::GapExtension, &first_proof);
    BlockAlignerExecutionProof changed_proof;
    const KswAlignResult changed = block_aligner_align_pair<false>(
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), dormant_changed,
        BlockAlignerPhase::GapExtension, &changed_proof);

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
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), changed_scoring,
        fixed_alignment_kernels<seq::dna4>(), first_params,
        BlockAlignerPhase::GapExtension, &scoring_proof);
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
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), first_params,
        BlockAlignerPhase::LeftExtension, &left_proof);
    BlockAlignerExecutionProof right_proof;
    const KswAlignResult projected_right = block_aligner_align_pair<false>(
        query_view, reference_view,
        0, query_data.size(), 0, reference_data.size(), nw1,
        fixed_alignment_kernels<seq::dna4>(), first_params,
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
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::as_lazy_sequence_view(lazy_target);
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
        query_view, target_view, scoring,
        fixed_alignment_kernels<seq::dna4>(), params, &proof);

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
    const auto query_view = bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::as_lazy_sequence_view(lazy_target);
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
            query_view, target_view, scoring,
            fixed_alignment_kernels<seq::dna4>(), params, &proof);
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
    SKIP("aa27 is outside the scheduler's generated alignment registry");
}

TEST_CASE("Block native L16 path matches the cross-lane golden",
          "[block-aligner][reference-golden][lane16]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 1, 1);
    const auto params = global_params(16, 32);
    const std::size_t lanes =
        fixed_alignment_kernels<seq::dna4>()
            .block_prepare(scoring, params).simd_lanes;
    if (lanes != 16) {
        SKIP("fixed generated target exposes L=8; L=16 requires AVX2");
    }

    const std::string sequence =
        "ACGTACGTACGTACGTTGCATGCATGCATGCA"
        "AAAACCCCGGGGTTTTACGTACGTACGTACGT";
    const ReferenceRun run = run_block_mode<false, false, dna4_u64>(
        sequence, sequence, scoring, params);
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
    const auto scoring = ScoringMatrix<dna4_u64>::generate(2, 1, 1);
    const std::size_t lanes =
        fixed_alignment_kernels<seq::dna4>()
            .block_prepare(scoring, global_params()).simd_lanes;
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

}  // namespace bioseq::simd
