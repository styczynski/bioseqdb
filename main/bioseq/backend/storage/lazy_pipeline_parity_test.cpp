// main/bioseq/backend/storage/lazy_pipeline_parity_test.cpp
//
// DOC0263 §11.3 / §11.3.1 — END-TO-END pipeline parity under fragmentation.
//
// Runs the FULL minimap_port pipeline (minimizer sketch -> seed hits -> chain ->
// region gen -> KSW2/block alignment -> mapq) over the SAME query/target through
// the no-fragment lazy wrapper and through chunked scanners, and asserts the
// MatchResult set (match count + per-match coordinates + CIGAR) is bit-identical.
//
// This exercises the chunked pipeline END-TO-END (the §11.3.1 intent) but
// instantiates the chunked scanner ONLY in this NO_POSTGRES test executable — not
// in the dispatch_buffer.cppm TU — so there is no production template explosion.
// The production end-to-end mmsearch path uses sequence_scanner_none (verified by
// the live psql proof in DOC0272 Appendix A); the chunked end-to-end path is
// proven here.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.minimap_port;   // MatchResult / MatchResultCollection
import bioseq.sequences.pipe;           // minimap_port entry
import bioseq.storage.scanner;
import bioseq.storage.lazy;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

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

// Canonicalise a MatchResultCollection into a comparable string set.
[[nodiscard]] std::vector<std::string> canon(const MatchResultCollection& c) {
    std::vector<std::string> out;
    for (const auto& m : c.results) {
        std::string cg;
        for (const auto& op : m.cigar)
            cg += fmt::format("{}:{},", op.length, static_cast<int>(op.opcode));
        out.push_back(fmt::format(
            "qs={} qe={} rev={} ts={} te={} nm={} bl={} mapq={} cs={} cg={}",
            m.query_start, m.query_end, m.is_reverse, m.target_start, m.target_end,
            m.num_matches, m.block_length, m.mapq, m.chain_score, cg));
    }
    std::sort(out.begin(), out.end());
    return out;
}

template <std::size_t FragUnits>
void check_pipeline_parity(std::size_t qn, std::size_t tn, std::uint64_t seed) {
    using chunked = bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, FragUnits>;
    auto qd = make_dna4(qn, seed);
    auto td = make_dna4(tn, seed ^ 0x5555);
    // Embed the query inside the target so a real alignment exists.
    const std::size_t embed = (tn > qn) ? (tn - qn) / 2 : 0;
    for (std::size_t i = 0; i < qn && embed + i < tn; ++i) td[embed + i] = qd[i];

    range_t q_range(qd);
    range_t t_range(td);

    // Baseline — no-fragment lazy wrappers.
    auto base_q = bioseq::storage::make_lazy_in_memory(q_range);
    auto base_t = bioseq::storage::make_lazy_in_memory(t_range);
    const auto baseline = canon(minimap_port(base_q, base_t, Minimap2Params{}));

    // Fragmented — chunked scanners (whole pipeline runs over fragments).
    bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> frag_q{chunked::from_range(q_range)};
    bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> frag_t{chunked::from_range(t_range)};
    const auto fragmented = canon(minimap_port(frag_q, frag_t, Minimap2Params{}));

    INFO("qn=" << qn << " tn=" << tn << " frag_units=" << FragUnits
               << " baseline_matches=" << baseline.size()
               << " fragmented_matches=" << fragmented.size());
    REQUIRE(baseline == fragmented);
    // Load-bearing: the alignment is non-trivial (not both-empty).
    REQUIRE_FALSE(baseline.empty());
}

}  // namespace

TEST_CASE("DOC0263 §11.3.1 — full minimap_port pipeline is bit-identical under a chunked scanner",
          "[lazy-fragmented][pipeline-parity]") {
    check_pipeline_parity<1>(120, 300, 0x1234);   // 32 letters / fragment (worst case)
    check_pipeline_parity<2>(120, 300, 0x1234);   // 64 letters / fragment
    check_pipeline_parity<32>(200, 600, 0xBEEF);  // 1024 letters / fragment (production size)
    check_pipeline_parity<32>(400, 1200, 0xC0DE);
}

TEST_CASE("DOC0286 — two_stack_nthash minimap_port handles small fragmented targets",
          "[lazy-fragmented][pipeline-parity][two-stack][doc0286]") {
    using chunked = bioseq::storage::sequence_scanner_test_chunked<
        std::uint64_t, bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;

    constexpr std::size_t query_bp = 5'000;
    constexpr std::size_t target_bp = 300'000;
    auto q_data = make_dna4(query_bp, 0x20260286);
    auto t_data = make_dna4(target_bp, 0x20260286 ^ 0x5555);
    constexpr std::size_t embed = 123'456;
    for (std::size_t i = 0; i < q_data.size(); ++i) {
        t_data[embed + i] = q_data[i];
    }

    range_t q_range(q_data);
    range_t t_range(t_data);

    Minimap2Params params{};
    params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    params.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
    params.minimizer_algorithm = MinimizerAlgorithmType::TWO_STACK_NTHASH;

    bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> frag_q{
        chunked::from_range(q_range)};
    bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> frag_t{
        chunked::from_range(t_range)};
    const auto matches = canon(minimap_port(frag_q, frag_t, params));

    REQUIRE_FALSE(matches.empty());
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
