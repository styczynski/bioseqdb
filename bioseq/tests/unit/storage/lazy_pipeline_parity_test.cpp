/**
 * @file
 * @brief Verifies minimap pipeline parity across resident and chunked sequence views.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <stdexcept>
#include <string>
#include <tests/support/common.hpp>
#include <vector>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

using namespace seq::literals;

namespace bioseq::simd {

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = compact_sequence_range<dna4_u64>;

/**
 * @brief Builds pipeline parameters bound to the generated fixed Highway target.
 * @return Parameters carrying a complete DNA4 kernel identity.
 * @throws std::logic_error If the generated binding lacks its DNA4 identity.
 */
[[nodiscard]] minimap2_parameters fixed_highway_params() {
    minimap2_parameters params;
    const auto& binding = ::bioseq::simd_dispatch::highway_test_target_binding();
    params.highway_kernels = binding.kernels;
    params.highway_executed_target = binding.target;
    const auto* identity = binding.kernels == nullptr || !binding.kernels->is_complete()
                               ? nullptr
                               : binding.kernels->resolve_alphabet_identity<seq::dna4>();
    if (identity == nullptr ||
        binding.kernels->resolve_alphabet_identity(identity->alphabet_code) != identity) {
        throw std::logic_error{"generated fixed-target test binding lacks DNA4 identity"};
    }
    params.alphabet_code = identity->alphabet_code;
    return params;
}

/**
 * @brief Generates deterministic DNA4 symbols from a linear-congruential stream.
 * @param[in] n Number of symbols to generate.
 * @param[in] seed Initial generator state.
 * @return Generated DNA4 sequence.
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
 * @brief Canonicalizes matches into sorted strings with all compared fields.
 * @param[in] collection Match collection to serialize.
 * @return Stable strings ordered independently of emission order.
 */
[[nodiscard]] std::vector<std::string> canon(const match_result_collection& collection) {
    std::vector<std::string> out;
    for (const auto& match : collection.results) {
        std::string cg;
        for (const auto& op : match.cigar)
            cg += fmt::format("{}:{},", op.length, static_cast<int>(op.opcode));
        out.push_back(fmt::format("qs={} qe={} rev={} ts={} te={} nm={} bl={} mapq={} cs={} cg={}",
                                  match.query_start,
                                  match.query_end,
                                  match.is_reverse,
                                  match.target_start,
                                  match.target_end,
                                  match.num_matches,
                                  match.block_length,
                                  match.mapq,
                                  match.chain_score,
                                  cg));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * @brief Compares one chunked pipeline execution with its resident baseline.
 * @tparam p_fragment_units Storage units exposed by each scanner fragment.
 * @param[in] qn Query length in symbols.
 * @param[in] tn Target length in symbols.
 * @param[in] seed Deterministic fixture seed.
 */
template <std::size_t p_fragment_units>
void check_pipeline_parity(std::size_t qn, std::size_t tn, std::uint64_t seed) {
    auto qd = make_dna4(qn, seed);
    auto td = make_dna4(tn, seed ^ 0x5555);
    // Embed the query inside the target so a real alignment exists.
    const std::size_t embed = (tn > qn) ? (tn - qn) / 2 : 0;
    for (std::size_t i = 0; i < qn && embed + i < tn; ++i)
        td[embed + i] = qd[i];

    range_t q_range(qd);
    range_t t_range(td);

    const auto baseline = canon(minimap_port(bioseq::storage::make_lazy_sequence_view(q_range),
                                             bioseq::storage::make_lazy_sequence_view(t_range),
                                             fixed_highway_params()));

    // Fragmented — exercise the same storage-owned facade callback used by
    // bioseq.testing.chunked_scanner.enabled, including the requested boundaries.
    std::vector<std::string> fragmented;
    bioseq::storage::with_chunked_lazy_sequence<p_fragment_units>(
        q_range, t_range, [&fragmented](auto query_view, auto target_view) {
            REQUIRE_FALSE(bioseq::storage::is_worker_shareable(query_view));
            REQUIRE_FALSE(bioseq::storage::is_worker_shareable(target_view));
            fragmented = canon(minimap_port(query_view, target_view, fixed_highway_params()));
        });

    INFO("qn=" << qn << " tn=" << tn << " frag_units=" << p_fragment_units << " baseline_matches="
               << baseline.size() << " fragmented_matches=" << fragmented.size());
    REQUIRE(baseline == fragmented);
    // Load-bearing: the alignment is non-trivial (not both-empty).
    REQUIRE_FALSE(baseline.empty());
}

} // namespace

TEST_CASE("full minimap_port pipeline is bit-identical under a chunked scanner",
          "[component:bioseq][layer:unit][feature:pipeline][resource:highway]") {
    check_pipeline_parity<1>(120, 300, 0x1234);  // 32 letters / fragment (worst case)
    check_pipeline_parity<2>(120, 300, 0x1234);  // 64 letters / fragment
    check_pipeline_parity<32>(200, 600, 0xBEEF); // 1024 letters / fragment (production size)
    check_pipeline_parity<32>(400, 1200, 0xC0DE);
}

TEST_CASE("two_stack_nthash minimap_port handles small fragmented targets",
          "[component:bioseq][layer:unit][feature:pipeline][resource:highway]") {
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

    minimap2_parameters params = fixed_highway_params();
    params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.minimizer_algorithm = minimizer_algorithm::two_stack_nthash;

    bioseq::storage::with_chunked_lazy_sequence<bioseq::storage::default_fragment_storage_units>(
        q_range, t_range, [&params](auto query, auto target) {
            const auto matches = canon(minimap_port(query, target, params));
            REQUIRE_FALSE(matches.empty());
        });
}

} // namespace bioseq::simd
