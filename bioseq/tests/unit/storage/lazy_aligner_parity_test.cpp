/**
 * @file
 * @brief Check that sequence  fragmentation preserves KSW2 scores and CIGAR operations.
 */

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.common.context;
import bioseq.storage.lazy_sequence;

using namespace seq::literals;

namespace bioseq::simd {

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = compact_sequence_range<dna4_u64>;

/**
 * @brief Get the complete DNA4 alignment table for this test's Highway target.
 * @throws std::logic_error If the DNA4 identity or alignment table is missing or incomplete.
 */
[[nodiscard]] const alignment_kernels<seq::dna4>& fixed_alignment_kernels() {
    const auto& root = ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity =
        root.is_complete() ? root.resolve_alphabet_identity<seq::dna4>() : nullptr;
    if (identity == nullptr ||
        root.resolve_alphabet_identity(identity->alphabet_code) != identity) {
        throw std::logic_error{"generated fixed-target test table lacks DNA4 identity"};
    }
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code && entry.typed_table_complete &&
            entry.typed_table != nullptr) {
            return *static_cast<const alignment_kernels<seq::dna4>*>(entry.typed_table);
        }
    }
    throw std::logic_error{"generated fixed-target test table lacks DNA4 alignment"};
}

/**
 * @brief Generate reproducible DNA4 bases.
 * @param[in] n Number of bases.
 * @param[in] seed Initial generator state.
 */
[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

/**
 * @brief Run ubanded single-affine KSW2 through the test's Highway table
 * @param[in] query Query view with storage valid for the call.
 * @param[in] target Target view with storage valid for  the call.
 * @param[in] query_length Number of query bases to align from position zero.
 * @param[in] target_length Number of target bases to align from position zero.
 * @param[in] scoring DNA4 scoring matrix.
 * @return Alignment score, coordinates, and CIGAR.
 */
[[nodiscard]] ksw_alignment_result run_ksw2(bioseq::storage::lazy_sequence_view<seq::dna4> query,
                                            bioseq::storage::lazy_sequence_view<seq::dna4> target,
                                            std::size_t query_length,
                                            std::size_t target_length,
                                            const scoring_matrix<dna4_u64>& scoring) {
    ksw_single_affine_parameters params;
    params.gap_open = 4;
    params.gap_extend = 2;
    params.band_width = -1;
    params.z_drop = -1;
    params.end_bonus = 0;
    return fixed_alignment_kernels().align_ksw2_single_affine(
        false, query, target, 0u, query_length, 0u, target_length, scoring, params);
}

} // namespace

TEST_CASE("KSW2 alignment is bit-identical under fragmentation",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);

    /// Query and target lengths, plus a seed for the input bases.
    struct parity_case {
        std::size_t qn, tn;
        std::uint64_t seed;
    };
    const std::array<parity_case, 4> cases{{
        {64, 64, 11},
        {120, 150, 22},
        {256, 240, 33},
        {500, 520, 44},
    }};

    for (auto [qn, tn, seed] : cases) {
        auto qd = make_dna4(qn, seed);
        // Give the query and target a shared sequence prefix.
        auto td = make_dna4(tn, seed);
        for (std::size_t i = 0; i + 3 < std::min(qn, tn); i += 1)
            td[i] = qd[i];

        range_t q_range(qd);
        range_t t_range(td);

        const auto baseline = run_ksw2(bioseq::storage::make_lazy_sequence_view(q_range),
                                       bioseq::storage::make_lazy_sequence_view(t_range),
                                       qn,
                                       tn,
                                       scoring);

        // Repeat the alignment with fragments of 1, 2, and 32 packed words.
        const auto require_chunked_parity =
            [&q_range, &t_range, qn, tn, &scoring, &baseline](auto fragment_units) {
                constexpr std::size_t units = decltype(fragment_units)::value;
                bioseq::storage::with_chunked_lazy_sequence<units>(
                    q_range, t_range, [qn, tn, &scoring, &baseline](auto query, auto target) {
                        const auto observed = run_ksw2(query, target, qn, tn, scoring);
                        REQUIRE(observed.score == baseline.score);
                        REQUIRE(observed.cigar == baseline.cigar);
                    });
            };
        require_chunked_parity(std::integral_constant<std::size_t, 1u>{});
        require_chunked_parity(std::integral_constant<std::size_t, 2u>{});
        require_chunked_parity(std::integral_constant<std::size_t, 32u>{});
    }
}

} // namespace bioseq::simd
