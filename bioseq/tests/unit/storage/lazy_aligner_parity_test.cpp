// Aligner CIGAR and score parity under fragmentation.
//
// The aligners (KSW2 / block aligner) consume canonical lazy views backed by
// live concrete owners. Since encoded access is byte-identical across
// scanners (locked by the lazy-sequence storage tests), the aligner output must
// be bit-identical too. This test runs the single-affine KSW2 kernel over the
// same query/target
// region through the no-fragment lazy wrapper and through three chunked
// scanners and asserts identical score + CIGAR.

#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

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
using range_t  = compact_sequence_range<dna4_u64>;

[[nodiscard]] const alignment_kernels<seq::dna4>&
fixed_alignment_kernels() {
    const auto& root =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity = root.is_complete()
        ? root.resolve_alphabet_identity<seq::dna4>() : nullptr;
    if (identity == nullptr
        || root.resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated fixed-target test table lacks DNA4 identity"};
    }
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code
            && entry.typed_table_complete
            && entry.typed_table != nullptr) {
            return *static_cast<const alignment_kernels<seq::dna4>*>(
                entry.typed_table);
        }
    }
    throw std::logic_error{
        "generated fixed-target test table lacks DNA4 alignment"};
}

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

[[nodiscard]] ksw_alignment_result run_ksw2(
        bioseq::storage::lazy_sequence_view<seq::dna4> query,
        bioseq::storage::lazy_sequence_view<seq::dna4> target,
        std::size_t query_length,
        std::size_t target_length,
        const scoring_matrix<dna4_u64>& scoring) {
    ksw_single_affine_parameters params;
    params.gap_open   = 4;
    params.gap_extend = 2;
    params.band_width = -1;
    params.z_drop     = -1;
    params.end_bonus  = 0;
    return fixed_alignment_kernels().align_ksw2_single_affine(
        false, query, target,
        0u, query_length, 0u, target_length, scoring, params);
}

}  // namespace

TEST_CASE("KSW2 alignment is bit-identical under fragmentation",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    const auto scoring = scoring_matrix<dna4_u64>::generate(2, 4, 1);

    struct parity_case { std::size_t qn, tn; std::uint64_t seed; };
    const std::array<parity_case, 4> cases{{
        {64, 64, 11}, {120, 150, 22},
        {256, 240, 33}, {500, 520, 44},
    }};

    for (auto [qn, tn, seed] : cases) {
        auto qd = make_dna4(qn, seed);
        // Make the target a mutated copy of the query so an alignment exists.
        auto td = make_dna4(tn, seed);
        for (std::size_t i = 0; i + 3 < std::min(qn, tn); i += 1) td[i] = qd[i];

        range_t q_range(qd);
        range_t t_range(td);

        const auto baseline = run_ksw2(
            bioseq::storage::make_lazy_sequence_view(q_range),
            bioseq::storage::make_lazy_sequence_view(t_range),
            qn, tn, scoring);

        // Fragmented: three chunk sizes (1 / 2 / 32 storage units per fragment).
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

}  // namespace bioseq::simd
