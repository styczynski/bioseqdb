#include "common.h"
#include "common_simd.h"
#include "common_tests.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

import bioseq.common.context;
import bioseq.sequences.chaining.scalar;
import bioseq.sequences.chaining.simd;
import bioseq.sequences.chaining.utils;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace {

namespace ch = ::bioseq::chaining;

TEST_CASE("ArenaSlab geometric growth is amortized and chain scopes release heap fallbacks",
          "[chaining][arena][memory]")
{
    context::context ctx;
    context::installed_context_scope installed{ctx};

    REQUIRE(ctx.chain_heap_fallback_bytes() == 0);
    {
        auto chain_scope = ctx.push<context::Arena::Chain>();
        auto slab = ch::make_arena_slab<std::uint64_t>();

        slab.reserve(140'000); // 1.12 MiB: force the oversize heap path.
        slab.resize(140'000);
        const auto first_capacity = slab.capacity();
        REQUIRE(ctx.chain_heap_fallback_bytes()
                == first_capacity * sizeof(std::uint64_t));

        slab.reserve_geometric(first_capacity + 1);
        REQUIRE(slab.capacity() == first_capacity * 2);
        REQUIRE(ctx.chain_heap_fallback_bytes()
                == (first_capacity + slab.capacity()) * sizeof(std::uint64_t));

        // Further append-sized requests within the doubled capacity must not
        // allocate or copy another prefix.
        const auto allocated_bytes = ctx.chain_heap_fallback_bytes();
        slab.reserve_geometric(first_capacity + 1024);
        REQUIRE(ctx.chain_heap_fallback_bytes() == allocated_bytes);
    }
    REQUIRE(ctx.chain_heap_fallback_bytes() == 0);
}

[[nodiscard]] ch::Anchor make_anchor(
    std::uint64_t ref,
    std::uint64_t query,
    std::uint8_t span = 15,
    std::uint32_t reverse = 0)
{
    return ch::Anchor{
        .ref_position = ref,
        .query_position = query,
        .is_reverse = reverse,
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

[[nodiscard]] std::vector<ch::Anchor> irregular_fixture(
    std::size_t count,
    std::uint64_t seed = 0xA57D1E5ULL)
{
    std::vector<ch::Anchor> anchors;
    anchors.reserve(count);
    std::uint64_t ref = (std::uint64_t{1} << 32) - 23;
    std::uint64_t query = 100;
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < count; ++i) {
        state = state * 6364136223846793005ULL
              + 1442695040888963407ULL;
        ref += 5 + ((state >> 9) % 19);
        state = state * 6364136223846793005ULL
              + 1442695040888963407ULL;
        query += 4 + ((state >> 13) % 17);
        anchors.push_back(make_anchor(
            ref,
            query,
            static_cast<std::uint8_t>(7 + ((state >> 21) % 14))));
    }
    return anchors;
}

[[nodiscard]] std::vector<ch::Anchor> diagonal_fixture(
    std::size_t count)
{
    std::vector<ch::Anchor> anchors;
    anchors.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        anchors.push_back(make_anchor(
            1000 + i * 10,
            200 + i * 10,
            10));
    }
    return anchors;
}

[[nodiscard]] ch::ChainingParams exact_params() {
    ch::ChainingParams params;
    params.max_gap = 1000;
    params.bandwidth = 250;
    params.max_chain_skip = 10000;
    params.max_chain_iter = 10000;
    params.min_seed_count = 1;
    params.min_chain_score = 0;
    params.chain_gap_scale = 0.8F;
    params.chain_skip_scale = 0.0F;
    params.prepare();
    return params;
}

struct ChainOutputs {
    std::vector<ch::Chain> chains;
    std::vector<ch::Anchor> anchors;
    SimdChainingExecutionProof proof{};
};

[[nodiscard]] ChainOutputs run_scalar(
    std::span<const ch::Anchor> anchors,
    const ch::ChainingParams& params)
{
    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains = ch::make_arena_slab<ch::Chain>();
    auto reordered = ch::make_arena_slab<ch::Anchor>();
    chains.reserve(anchors.size() + 1);
    reordered.reserve(anchors.size() + 1);
    ch::chain_anchors_scalar(anchors, params, chains, reordered);
    ChainOutputs result;
    result.chains.assign(
        chains.data(), chains.data() + chains.size());
    result.anchors.assign(
        reordered.data(), reordered.data() + reordered.size());
    return result;
}

[[nodiscard]] ChainOutputs run_simd(
    std::span<const ch::Anchor> anchors,
    const ch::ChainingParams& params)
{
    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains = ch::make_arena_slab<ch::Chain>();
    auto reordered = ch::make_arena_slab<ch::Anchor>();
    chains.reserve(anchors.size() + 1);
    reordered.reserve(anchors.size() + 1);
    ChainOutputs result;
    chain_anchors_simd(
        anchors, params, chains, reordered, &result.proof);
    result.chains.assign(
        chains.data(), chains.data() + chains.size());
    result.anchors.assign(
        reordered.data(), reordered.data() + reordered.size());
    return result;
}

void require_same_outputs(
    const ChainOutputs& actual,
    const ChainOutputs& expected,
    const std::string& fixture)
{
    INFO("fixture: " << fixture);
    REQUIRE(actual.chains.size() == expected.chains.size());
    REQUIRE(actual.anchors.size() == expected.anchors.size());
    for (std::size_t i = 0; i < expected.chains.size(); ++i) {
        INFO("chain index: " << i);
        REQUIRE(actual.chains[i].score == expected.chains[i].score);
        REQUIRE(
            actual.chains[i].anchor_count
            == expected.chains[i].anchor_count);
        REQUIRE(
            actual.chains[i].anchor_start
            == expected.chains[i].anchor_start);
    }
    for (std::size_t i = 0; i < expected.anchors.size(); ++i) {
        INFO("reordered anchor index: " << i);
        const auto& lhs = actual.anchors[i];
        const auto& rhs = expected.anchors[i];
        REQUIRE(lhs.ref_position == rhs.ref_position);
        REQUIRE(lhs.query_position == rhs.query_position);
        REQUIRE(lhs.is_reverse == rhs.is_reverse);
        REQUIRE(lhs.query_span == rhs.query_span);
        REQUIRE(lhs.flags == rhs.flags);
        REQUIRE(lhs.segment_id == rhs.segment_id);
    }
}

void require_no_forbidden_scalar_work(
    const SimdChainingExecutionProof& proof)
{
    REQUIRE(proof.scalar_score_branches == 0);
    REQUIRE(proof.scalar_backtrack_branches == 0);
    REQUIRE(proof.shared_scalar_backtrack_calls == 0);
    REQUIRE(proof.fail_closed_rejections == 0);
}

}  // namespace

TEST_CASE(
    "exact SIMD chaining scores every small range and masked tail",
    "[chaining][simd][exact][small][tail]")
{
    hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    REQUIRE(lanes >= 2);

    bool observed_masked_batch = false;
    const std::size_t largest = 2 * lanes + 7;
    for (std::size_t count = 1; count <= largest; ++count) {
        const auto anchors = irregular_fixture(count, 0x1000 + count);
        const auto params = exact_params();
        const auto expected = run_scalar(anchors, params);
        const auto actual = run_simd(anchors, params);
        require_same_outputs(
            actual, expected, "small/tail n=" + std::to_string(count));
        require_no_forbidden_scalar_work(actual.proof);
        REQUIRE(actual.proof.anchors_consumed == count);
        if (count > 1) {
            REQUIRE(actual.proof.score_vector_batches > 0);
            REQUIRE(actual.proof.score_vector_lanes > 0);
            REQUIRE(actual.proof.descending_candidate_steps > 0);
        }
        observed_masked_batch =
            observed_masked_batch
            || actual.proof.score_masked_lanes > 0;
    }
    REQUIRE(observed_masked_batch);
}

TEST_CASE(
    "exact SIMD chaining includes nonzero skip penalty",
    "[chaining][simd][exact][skip_penalty]")
{
    auto params = exact_params();
    params.chain_skip_scale = 3.25F;
    params.prepare();
    REQUIRE(params.skip_penalty > 0.0F);
    const auto anchors = irregular_fixture(37, 0x5A17);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "nonzero skip penalty");
    require_no_forbidden_scalar_work(actual.proof);
    REQUIRE(actual.proof.score_vector_batches > 0);
}

TEST_CASE(
    "exact SIMD chaining preserves descending visited max-skip stop",
    "[chaining][simd][exact][visited][max_skip]")
{
    auto params = exact_params();
    params.max_chain_skip = 0;
    const auto anchors = diagonal_fixture(41);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "visited stop max_skip=0");
    require_no_forbidden_scalar_work(actual.proof);
    REQUIRE(actual.proof.visited_marks > 0);
    REQUIRE(actual.proof.max_chain_skip_stops > 0);
}

TEST_CASE(
    "exact SIMD chaining preserves ties parents and output order",
    "[chaining][simd][exact][ties][parents][order]")
{
    auto params = exact_params();
    params.max_chain_skip = 10000;
    const auto anchors = diagonal_fixture(29);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "equal-score predecessor ties");
    require_no_forbidden_scalar_work(actual.proof);

    REQUIRE(actual.chains.size() == 1);
    REQUIRE(
        actual.chains.front().anchor_count
        == static_cast<std::int32_t>(anchors.size()));
    REQUIRE(actual.anchors.size() == anchors.size());
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        // The strict-greater predecessor policy keeps the nearest descending
        // predecessor on ties, yielding the complete parent chain in order.
        REQUIRE(
            actual.anchors[i].ref_position
            == anchors[i].ref_position);
        REQUIRE(
            actual.anchors[i].query_position
            == anchors[i].query_position);
    }
}

TEST_CASE(
    "lane-batched SIMD backtracking resolves overlapping paths exactly",
    "[chaining][simd][exact][backtrack][overlap]")
{
    auto params = exact_params();
    params.min_chain_score = 0;
    params.min_seed_count = 1;
    const auto anchors = diagonal_fixture(53);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "overlapping ranked paths");
    require_no_forbidden_scalar_work(actual.proof);
    REQUIRE(actual.proof.backtrack_rank_batches > 0);
    REQUIRE(actual.proof.backtrack_path_vector_batches > 0);
    REQUIRE(actual.proof.backtrack_commit_vector_batches > 0);
    REQUIRE(actual.proof.backtrack_vector_lanes > 0);
    REQUIRE(actual.proof.backtrack_overlap_stops > 0);
}

TEST_CASE(
    "exact SIMD chaining rejects unsupported parameters without fallback",
    "[chaining][simd][exact][fail_closed]")
{
    auto params = exact_params();
    params.max_chain_iter = -1;
    const auto anchors = diagonal_fixture(8);

    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains = ch::make_arena_slab<ch::Chain>();
    auto reordered = ch::make_arena_slab<ch::Anchor>();
    chains.reserve(anchors.size() + 1);
    reordered.reserve(anchors.size() + 1);
    SimdChainingExecutionProof proof;
    try {
        chain_anchors_simd(
            anchors, params, chains, reordered, &proof);
        FAIL("invalid Exact SIMD parameters must throw");
    } catch (const SimdChainingRuntimeError& error) {
        REQUIRE(
            error.status()
            == SimdChainingStatus::invalid_parameters);
    }
    REQUIRE(proof.fail_closed_rejections == 1);
    REQUIRE(proof.scalar_score_branches == 0);
    REQUIRE(proof.scalar_backtrack_branches == 0);
    REQUIRE(proof.shared_scalar_backtrack_calls == 0);
    REQUIRE(chains.size() == 0);
    REQUIRE(reordered.size() == 0);
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
