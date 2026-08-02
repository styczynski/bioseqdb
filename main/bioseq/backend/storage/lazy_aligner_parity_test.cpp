// main/bioseq/backend/storage/lazy_aligner_parity_test.cpp
//
// DOC0263 §11.3 — aligner CIGAR/score parity under fragmentation.
//
// The aligners (KSW2 / block aligner) consume canonical lazy views backed by
// live concrete owners. Since encoded access is byte-identical across
// scanners (locked by lazy_compact_sequence_test.cpp), the aligner output must
// be bit-identical too. This test runs KSW2 z2_align over the SAME query/target
// region through the no-fragment lazy wrapper and through three chunked
// scanners and asserts identical score + CIGAR.

#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.alignment;            // KswZ2Params, KswAlignResult
import bioseq.alignment.utils;      // ScoringMatrix
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy;

using namespace seq::literals;

namespace bioseq::simd {

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

[[nodiscard]] const AlignmentKernels<seq::dna4>&
fixed_alignment_kernels() {
    const auto& root =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    for (const auto& entry : root.alignment_by_alphabet) {
        if (entry.alphabet_code == 0u && entry.typed_table != nullptr) {
            return *static_cast<const AlignmentKernels<seq::dna4>*>(
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

template <class LazyQ, class LazyT>
[[nodiscard]] KswAlignResult run_ksw2(LazyQ& q, LazyT& t,
                                      std::size_t qlen, std::size_t tlen,
                                      const ScoringMatrix<dna4_u64>& sm) {
    KswZ2Params params;
    params.gap_open   = 4;
    params.gap_extend = 2;
    params.band_width = -1;
    params.z_drop     = -1;
    params.end_bonus  = 0;
    const auto query_view = bioseq::storage::as_lazy_sequence_view(q);
    const auto target_view = bioseq::storage::as_lazy_sequence_view(t);
    return fixed_alignment_kernels().ksw2_z2(
        false, query_view, target_view, 0, qlen, 0, tlen, sm, params);
}

}  // namespace

TEST_CASE("DOC0263 §11.3 — KSW2 alignment is bit-identical under fragmentation",
          "[lazy-fragmented][aligner-parity]") {
    context::context ctx;
    context::installed_context_scope scope(ctx);

    const auto sm = ScoringMatrix<dna4_u64>::generate(2, 4, 1);

    struct Case { std::size_t qn, tn; std::uint64_t seed; };
    const std::array<Case, 4> cases{{{64, 64, 11}, {120, 150, 22}, {256, 240, 33}, {500, 520, 44}}};

    for (auto [qn, tn, seed] : cases) {
        auto qd = make_dna4(qn, seed);
        // Make the target a mutated copy of the query so an alignment exists.
        auto td = make_dna4(tn, seed);
        for (std::size_t i = 0; i + 3 < std::min(qn, tn); i += 1) td[i] = qd[i];

        range_t q_range(qd);
        range_t t_range(td);

        // Baseline: no-fragment lazy wrapper.
        auto lq = bioseq::storage::make_lazy_in_memory(q_range);
        auto lt = bioseq::storage::make_lazy_in_memory(t_range);
        const KswAlignResult baseline = run_ksw2(lq, lt, qn, tn, sm);

        // Fragmented: three chunk sizes (1 / 2 / 32 storage units per fragment).
        {
            using chunked = bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, 1>;
            bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> fq{chunked::from_range(q_range)};
            bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> ft{chunked::from_range(t_range)};
            const KswAlignResult got = run_ksw2(fq, ft, qn, tn, sm);
            REQUIRE(got.score == baseline.score);
            REQUIRE(got.cigar == baseline.cigar);
        }
        {
            using chunked = bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, 2>;
            bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> fq{chunked::from_range(q_range)};
            bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> ft{chunked::from_range(t_range)};
            const KswAlignResult got = run_ksw2(fq, ft, qn, tn, sm);
            REQUIRE(got.score == baseline.score);
            REQUIRE(got.cigar == baseline.cigar);
        }
        {
            using chunked = bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, 32>;
            bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> fq{chunked::from_range(q_range)};
            bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> ft{chunked::from_range(t_range)};
            const KswAlignResult got = run_ksw2(fq, ft, qn, tn, sm);
            REQUIRE(got.score == baseline.score);
            REQUIRE(got.cigar == baseline.cigar);
        }
    }
}

}  // namespace bioseq::simd
