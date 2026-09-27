/**
 * @file test_support.cppm
 * @brief Builds sequence fixtures and compares block alignment results.
 */
module;

#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.unit.alignment.block_aligner_support;

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
export namespace bioseq::simd::block_aligner_test_support {

/** DNA4 packed into 64-bit words for the alignment tests. */
using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
/** Packed DNA4 range wrapped by the in-memory sequence fixtures. */
using range_t = compact_sequence_range<dna4_u64>;
/** DNA5 packed into 64-bit words for tests with ambiguous bases. */
using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;

/**
 * Returns the alphabet's alignment kernels for,  the test CPU target.
 * Throws std::logic_error if the generated table has no matching entry.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] const alignment_kernels<p_alphabet>& fixed_alignment_kernels() {
    const auto& root = ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity =
        root.is_complete() ? root.template resolve_alphabet_identity<p_alphabet>() : nullptr;
    if (identity == nullptr ||
        root.resolve_alphabet_identity(identity->alphabet_code) != identity) {
        throw std::logic_error{"generated fixed-target test table lacks alphabet identity"};
    }
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code && entry.typed_table != nullptr) {
            return *static_cast<const alignment_kernels<p_alphabet>*>(entry.typed_table);
        }
    }
    throw std::logic_error{"generated fixed-target test table lacks alignment alphabet"};
}

/** Keeps an alignment result with its execution and allocation counters. */
struct reference_run {
    ksw_alignment_result result;
    block_aligner_execution_stats stats;
    block_aligner_construction_stats construction;
};

/** @brief Converts ASCII bases into DNA4 symbols for a fixture. */
[[nodiscard]] std::vector<seq::dna4> dna4_from_ascii(std::string_view text) {
    std::vector<seq::dna4> result(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        result[i].assign_char(text[i]);
    }
    return result;
}

/** Converts packed CIGAR words to text for test comparisons. */
[[nodiscard]] std::string packed_cigar_string(std::span<const std::uint32_t> cigar) {
    std::string result;
    for (const std::uint32_t packed : cigar) {
        result += std::to_string(packed >> 4u);
        switch (packed & 0xfu) {
        case cigar_match_opcode:
            result += 'M';
            break;
        case cigar_insertion_opcode:
            result += 'I';
            break;
        case cigar_deletion_opcode:
            result += 'D';
            break;
        default:
            result += '?';
            break;
        }
    }
    return result;
}

/** Builds global gap-fill settings with the requested block sizes and traceback option. */
[[nodiscard]] block_aligner_parameters
global_params(std::size_t min_size = 16, std::size_t max_size = 16, bool trace = true) {
    return block_aligner_parameters{
        .gap_open = 1,
        .gap_extend = 1,
        .minimum_block_size = min_size,
        .maximum_block_size = max_size,
        .x_drop_threshold = 0,
        .mode = block_alignment_mode::global,
        .input_order = block_input_order::forward,
        .stage = block_alignment_stage::gap_extension,
        .is_trace_enabled = trace,
    };
}

/** Builds X-drop extension settings for the given stage and input orer. */
[[nodiscard]] block_aligner_parameters
xdrop_params(block_alignment_stage stage, block_input_order input_order, bool trace = true) {
    return block_aligner_parameters{
        .gap_open = 10,
        .gap_extend = 1,
        .minimum_block_size = 16,
        .maximum_block_size = 16,
        .x_drop_threshold = 1,
        .mode = block_alignment_mode::x_drop,
        .input_order = input_order,
        .stage = stage,
        .is_trace_enabled = trace,
    };
}

/** Aligns complete DNA4 inputs and allows the diagonal shortcut. */
[[nodiscard]] reference_run run_block(std::string_view query,
                                      std::string_view reference,
                                      const scoring_matrix<dna4_u64>& scoring,
                                      const block_aligner_parameters& params) {
    auto query_data = dna4_from_ascii(query);
    auto reference_data = dna4_from_ascii(reference);
    range_t query_range(query_data);
    range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_reference = bioseq::storage::make_resident_lazy_sequence(reference_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto reference_view = bioseq::storage::make_lazy_sequence_view(lazy_reference);

    reference_run run;
    run.result = fixed_alignment_kernels<seq::dna4>().align_block(false,
                                                                  true,
                                                                  query_view,
                                                                  reference_view,
                                                                  0,
                                                                  query.size(),
                                                                  0,
                                                                  reference.size(),
                                                                  scoring,
                                                                  params,
                                                                  &run.stats,
                                                                  &run.construction);
    return run;
}

/**
 * Aligns selected regions and records the work performed.
 * @tparam p_strand_reversed Read the query as a reverse complement.
 * @tparam p_enable_diagonal_certificate Allow a score bound to prove that an ungapped alignment is optimal.
 * @tparam p_sequence Packed sequence type used by the fixture.
 */
template <bool p_strand_reversed, bool p_enable_diagonal_certificate, class p_sequence>
[[nodiscard]] reference_run run_block_region_mode(std::string_view query_source,
                                                  std::string_view reference_source,
                                                  std::size_t query_start,
                                                  std::size_t query_end,
                                                  std::size_t reference_start,
                                                  std::size_t reference_end,
                                                  const scoring_matrix<p_sequence>& scoring,
                                                  const block_aligner_parameters& params) {
    using typed_range_t = compact_sequence_range<p_sequence>;
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
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_reference = bioseq::storage::make_resident_lazy_sequence(reference_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto reference_view = bioseq::storage::make_lazy_sequence_view(lazy_reference);

    reference_run run;
    run.result = fixed_alignment_kernels<alphabet_t>().align_block(p_strand_reversed,
                                                                   p_enable_diagonal_certificate,
                                                                   query_view,
                                                                   reference_view,
                                                                   query_start,
                                                                   query_end,
                                                                   reference_start,
                                                                   reference_end,
                                                                   scoring,
                                                                   params,
                                                                   &run.stats,
                                                                   &run.construction);
    return run;
}

/**
 * Aligns both  full inputs with the selected strand and diagonal-shortcut setting.
 * @tparam p_strand_reversed Read the query as a reverse complement.
 * @tparam p_enable_diagonal_certificate Allow a score bound to prove that an ungapped alignment is optimal.
 * @tparam p_sequence Packed sequence type used by the fixture.
 */
template <bool p_strand_reversed, bool p_enable_diagonal_certificate, class p_sequence>
[[nodiscard]] reference_run run_block_mode(std::string_view query,
                                           std::string_view reference,
                                           const scoring_matrix<p_sequence>& scoring,
                                           const block_aligner_parameters& params) {
    return run_block_region_mode<p_strand_reversed, p_enable_diagonal_certificate, p_sequence>(
        query, reference, 0, query.size(), 0, reference.size(), scoring, params);
}

/** Checks result scores, endpoints, status flags and packed CIGAR for equality. */
void check_align_result_identical(const ksw_alignment_result& optimized,
                                  const ksw_alignment_result& forced_dp) {
    CHECK(optimized.max == forced_dp.max);
    CHECK(optimized.score == forced_dp.score);
    CHECK(optimized.mqe == forced_dp.mqe);
    CHECK(optimized.mte == forced_dp.mte);
    CHECK(optimized.max_q == forced_dp.max_q);
    CHECK(optimized.max_t == forced_dp.max_t);
    CHECK(optimized.mqe_t == forced_dp.mqe_t);
    CHECK(optimized.mte_q == forced_dp.mte_q);
    CHECK(optimized.is_z_dropped == forced_dp.is_z_dropped);
    CHECK(optimized.has_reached_end == forced_dp.has_reached_end);
    CHECK(optimized.cigar == forced_dp.cigar);
}

/** Reverses the text and complements uppercase A, C, G and T */
[[nodiscard]] std::string reverse_complement_ascii(std::string_view source) {
    std::string result(source.rbegin(), source.rend());
    for (char& symbol : result) {
        switch (symbol) {
        case 'A':
            symbol = 'T';
            break;
        case 'C':
            symbol = 'G';
            break;
        case 'G':
            symbol = 'C';
            break;
        case 'T':
            symbol = 'A';
            break;
        default:
            break;
        }
    }
    return result;
}

/** Aligns selected DNA4 regions and allows the diagonal shortcut. */
[[nodiscard]] reference_run run_block_region(std::string_view query_source,
                                             std::string_view reference_source,
                                             std::size_t query_start,
                                             std::size_t query_end,
                                             std::size_t reference_start,
                                             std::size_t reference_end,
                                             const scoring_matrix<dna4_u64>& scoring,
                                             const block_aligner_parameters& params) {
    auto query_data = dna4_from_ascii(query_source);
    auto reference_data = dna4_from_ascii(reference_source);
    range_t query_range(query_data);
    range_t reference_range(reference_data);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_reference = bioseq::storage::make_resident_lazy_sequence(reference_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto reference_view = bioseq::storage::make_lazy_sequence_view(lazy_reference);

    reference_run run;
    run.result = fixed_alignment_kernels<seq::dna4>().align_block(false,
                                                                  true,
                                                                  query_view,
                                                                  reference_view,
                                                                  query_start,
                                                                  query_end,
                                                                  reference_start,
                                                                  reference_end,
                                                                  scoring,
                                                                  params,
                                                                  &run.stats,
                                                                  &run.construction);
    return run;
}

/** @brief Creates an anchor with equal query and reference positions. */
[[nodiscard]] ::bioseq::chaining::anchor make_anchor(std::uint64_t position,
                                                     std::uint8_t span = 15) {
    return ::bioseq::chaining::anchor{
        .ref_position = position,
        .query_position = position,
        .is_reverse = 0,
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

/** @brief Creates an anchor with independent query and reference positions. */
[[nodiscard]] ::bioseq::chaining::anchor make_anchor_at(std::uint64_t reference_position,
                                                        std::uint64_t query_position,
                                                        std::uint8_t span = 1) {
    return ::bioseq::chaining::anchor{
        .ref_position = reference_position,
        .query_position = query_position,
        .is_reverse = 0,
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

/** Compares region coordinates, scores, alignment status and CIGAR text. */
void check_region_alignment_identical(const alignment_region& optimized,
                                      const alignment_region& forced_dp) {
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

/** Scores and a label used to generate a test scoring matrix. */
struct scoring_matrix_generation_case {
    std::int8_t match;
    std::int8_t mismatch;
    std::int8_t ambiguity;
    std::string_view description;
};

/** Scoring cases that  check signed penalties, zero values and int8 limits. */
constexpr std::array<scoring_matrix_generation_case, 6> scoring_matrix_generation_cases{{
    {4, 6, 1, "positive penalty magnitudes"},
    {-4, -6, -1, "fully signed scores"},
    {7, -3, -2, "positive match and signed penalties"},
    {-7, 3, 2, "signed match and positive penalties"},
    {0, 0, 0, "zero scores"},
    {127, 126, 125, "positive int8 limits"},
}};

/**
 * Checks that alphabet and packed-sequence matrix types give the same scores.
 * Also checks SIMD padding and how signed penalties are normalized.
 * @tparam p_alphabet Alphabet used by both matrix types.
 */
template <class p_alphabet>
void check_scoring_matrix_facade_parity(std::string_view alphabet_name) {
    using adapted_source = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    using canonical_matrix = scoring_matrix<p_alphabet>;
    using adapted_matrix = scoring_matrix<adapted_source>;

    static_assert(std::is_same_v<canonical_matrix, adapted_matrix>);
    static_assert(std::is_same_v<typename canonical_matrix::alphabet_type, p_alphabet>);
    static_assert(canonical_matrix::k_alphabet_dimension == adapted_matrix::k_alphabet_dimension);
    static_assert(canonical_matrix::k_simd_rows == adapted_matrix::k_simd_rows);
    static_assert(canonical_matrix::k_simd_columns == adapted_matrix::k_simd_columns);
    static_assert(canonical_matrix::k_null_byte == adapted_matrix::k_null_byte);

    constexpr auto sentinel = std::numeric_limits<std::int8_t>::min();
    constexpr std::size_t dim = canonical_matrix::k_alphabet_dimension;
    static_assert(dim > 1u);
    static_assert(canonical_matrix::k_simd_rows == dim + 1u);
    static_assert(canonical_matrix::k_simd_columns >= dim + 1u);
    static_assert(canonical_matrix::k_null_byte == dim);

    for (const auto& generation : scoring_matrix_generation_cases) {
        const auto canonical =
            canonical_matrix::generate(generation.match, generation.mismatch, generation.ambiguity);
        const auto adapted =
            adapted_matrix::generate(generation.match, generation.mismatch, generation.ambiguity);

        INFO("alphabet=" << alphabet_name << " score-case=" << generation.description << " scores=("
                         << static_cast<int>(generation.match) << ','
                         << static_cast<int>(generation.mismatch) << ','
                         << static_cast<int>(generation.ambiguity) << ')');
        REQUIRE(canonical.is_block_layout_valid());
        REQUIRE(adapted.is_block_layout_valid());
        CHECK(canonical.block_semantic_identity() == adapted.block_semantic_identity());
        CHECK(canonical.match_score() == adapted.match_score());
        CHECK(canonical.mismatch_score() == adapted.mismatch_score());
        CHECK(canonical.wildcard_score() == adapted.wildcard_score());

        const auto expected_match =
            generation.match > 0 ? generation.match : static_cast<std::int8_t>(-generation.match);
        const auto expected_mismatch = generation.mismatch > 0
                                           ? static_cast<std::int8_t>(-generation.mismatch)
                                           : generation.mismatch;
        const auto expected_ambiguity = generation.ambiguity > 0
                                            ? static_cast<std::int8_t>(-generation.ambiguity)
                                            : generation.ambiguity;

        for (std::size_t row = 0; row < dim; ++row) {
            for (std::size_t col = 0; col < dim; ++col) {
                INFO("scalar-cell row=" << row << " col=" << col);
                const auto expected = row == dim - 1u || col == dim - 1u ? expected_ambiguity
                                      : row == col                       ? expected_match
                                                                         : expected_mismatch;
                CHECK(canonical.at(static_cast<int>(row), static_cast<int>(col)) == expected);
                CHECK(adapted.at(static_cast<int>(row), static_cast<int>(col)) == expected);
                CHECK(adapted.at(static_cast<int>(row), static_cast<int>(col)) ==
                      canonical.at(static_cast<int>(row), static_cast<int>(col)));
            }
        }

        for (std::size_t row = 0; row < canonical_matrix::k_simd_rows; ++row) {
            for (std::size_t col = 0; col < canonical_matrix::k_simd_columns; ++col) {
                INFO("simd-cell row=" << row << " col=" << col);
                const auto expected = row < dim && col < dim ? canonical.at(static_cast<int>(row),
                                                                            static_cast<int>(col))
                                                             : sentinel;
                CHECK(canonical.simd_row_pointer(row)[col] == expected);
                CHECK(adapted.simd_row_pointer(row)[col] == expected);
                CHECK(adapted.simd_row_pointer(row)[col] == canonical.simd_row_pointer(row)[col]);
            }
        }
    }
}
} // namespace bioseq::simd::block_aligner_test_support
