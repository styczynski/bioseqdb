#include "common.h"
#include "common_simd.h"
#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

import bioseq.common.context;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences.chaining.scalar;
import bioseq.sequences.chaining.simd;
import bioseq.sequences.chaining.utils;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace {

namespace ch = ::bioseq::chaining;
using ch::SimdChainingExecutionProof;
using ch::SimdChainingRuntimeError;
using ch::SimdChainingStatus;

[[nodiscard]] ch::ChainAnchorsSimdKernel fixed_chaining_kernel()
{
    const auto kernel = ::bioseq::simd_dispatch::highway_test_kernel_table()
        .chaining.chain_anchors_simd;
    if (kernel == nullptr) {
        throw std::logic_error{
            "generated fixed-target test table lacks SIMD chaining"};
    }
    return kernel;
}

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

[[nodiscard]] std::vector<ch::Anchor> older_winner_fixture(
    std::size_t main_count,
    std::size_t distractor_count)
{
    constexpr std::uint64_t current_ref = 10000;
    constexpr std::uint64_t current_query = 10000;
    constexpr std::size_t distractor_positions = 32u;
    std::vector<ch::Anchor> anchors;
    anchors.reserve(main_count + distractor_count + 1u);

    const std::uint64_t main_ref_end = current_ref - 100u;
    const std::uint64_t main_query_end = current_query - 100u;
    for (std::size_t i = 0; i < main_count; ++i) {
        const std::uint64_t distance =
            static_cast<std::uint64_t>(main_count - i - 1u) * 10u;
        anchors.push_back(make_anchor(
            main_ref_end - distance,
            main_query_end - distance,
            1));
    }

    const std::size_t repetitions =
        distractor_count / distractor_positions;
    for (std::size_t position = 0;
         position < distractor_positions; ++position)
    {
        for (std::size_t repeat = 0; repeat < repetitions; ++repeat) {
            anchors.push_back(make_anchor(
                current_ref - distractor_positions + position,
                current_query - 9000u,
                1));
        }
    }
    anchors.push_back(make_anchor(
        current_ref, current_query, 64));
    return anchors;
}

[[nodiscard]] std::vector<ch::Anchor> equal_score_tie_fixture(
    std::size_t group_count)
{
    std::vector<ch::Anchor> anchors;
    anchors.reserve(group_count * 2u);
    for (std::size_t group = 0; group < group_count; ++group) {
        for (std::uint8_t duplicate = 0; duplicate < 2u; ++duplicate) {
            auto anchor = make_anchor(
                1000u + group * 8u,
                1000u + group * 8u,
                8);
            anchor.flags = duplicate;
            anchors.push_back(anchor);
        }
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
    params.simd_policy = ch::SimdChainingPolicy::exact_visited;
    params.prepare();
    return params;
}

[[nodiscard]] ch::ChainingParams bounded_params() {
    auto params = exact_params();
    params.max_chain_skip = 25;
    params.simd_policy =
        ch::SimdChainingPolicy::bounded_batch_approximate;
    return params;
}

/// An older exact winner that is not the active-window global-best score.
/// The intervening off-diagonal chain can be extended arbitrarily, so a
/// bounded batch cutoff has no theorem-level endpoint-error bound. This is a
/// counterexample fixture, not endpoint-qualification corpus data.
[[nodiscard]] std::vector<ch::Anchor> theorem_counterexample_fixture() {
    std::vector<ch::Anchor> anchors;
    anchors.reserve(221u);

    // Exact winner: score 100, but a 64-span final anchor contributes 64 to
    // the last current-anchor edge and makes the resulting chain score 164.
    for (std::size_t i = 0; i < 100u; ++i) {
        anchors.push_back(make_anchor(
            7000u + i * 10u,
            7000u + i * 10u,
            i == 99u ? 64u : 1u));
    }

    // Active-window global best: score 120. It is strongly off diagonal from
    // the final anchor, so the rescue probe cannot use it.
    for (std::size_t i = 0; i < 120u; ++i) {
        anchors.push_back(make_anchor(
            8500u + i * 10u,
            1000u + i * 10u,
            1u));
    }
    anchors.push_back(make_anchor(10000u, 10000u, 1u));
    return anchors;
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
    const ch::ChainingParams& params,
    bool aggregate_proof = false)
{
    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains = ch::make_arena_slab<ch::Chain>();
    auto reordered = ch::make_arena_slab<ch::Anchor>();
    chains.reserve(anchors.size() + 1);
    reordered.reserve(anchors.size() + 1);
    ChainOutputs result;
    ch::chain_anchors_simd(
        fixed_chaining_kernel(),
        anchors,
        params,
        chains,
        reordered,
        &result.proof,
        aggregate_proof);
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

void require_selected_policy(
    const SimdChainingExecutionProof& proof,
    ch::SimdChainingPolicy selected,
    bool require_accelerated_work = false)
{
    REQUIRE(proof.policy_evidence_coherent(
        selected, require_accelerated_work));
    switch (selected) {
        case ch::SimdChainingPolicy::exact_visited:
            REQUIRE(proof.policy.exact_visited_calls == 1);
            REQUIRE(
                proof.policy.bounded_batch_approximate_calls == 0);
            break;
        case ch::SimdChainingPolicy::bounded_batch_approximate:
            REQUIRE(proof.policy.exact_visited_calls == 0);
            REQUIRE(
                proof.policy.bounded_batch_approximate_calls == 1);
            break;
    }
}

[[nodiscard]] bool outputs_differ(
    const ChainOutputs& lhs,
    const ChainOutputs& rhs)
{
    if (lhs.chains.size() != rhs.chains.size()
        || lhs.anchors.size() != rhs.anchors.size())
    {
        return true;
    }
    for (std::size_t i = 0; i < lhs.chains.size(); ++i) {
        if (lhs.chains[i].score != rhs.chains[i].score
            || lhs.chains[i].anchor_count
                != rhs.chains[i].anchor_count
            || lhs.chains[i].anchor_start
                != rhs.chains[i].anchor_start)
        {
            return true;
        }
    }
    for (std::size_t i = 0; i < lhs.anchors.size(); ++i) {
        if (lhs.anchors[i].ref_position
                != rhs.anchors[i].ref_position
            || lhs.anchors[i].query_position
                != rhs.anchors[i].query_position
            || lhs.anchors[i].flags != rhs.anchors[i].flags)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE(
    "SIMD chaining policy defaults exact and has stable names",
    "[chaining][simd][policy][default][names]")
{
    const ch::ChainingParams params{};
    REQUIRE(
        params.simd_policy
        == ch::SimdChainingPolicy::exact_visited);
    REQUIRE(
        ch::simd_chaining_policy_name(
            ch::SimdChainingPolicy::exact_visited)
        == "exact_visited");
    REQUIRE(
        ch::simd_chaining_policy_name(
            ch::SimdChainingPolicy::bounded_batch_approximate)
        == "bounded_batch_approximate");
    REQUIRE(
        ch::simd_chaining_policy_name(
            static_cast<ch::SimdChainingPolicy>(255u))
        == "unknown");
}

TEST_CASE(
    "bounded SIMD chaining uses Highway for full and masked batches",
    "[chaining][simd][bounded][small][tail][proof]")
{
    hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    REQUIRE(lanes >= 2u);

    bool observed_masked_batch = false;
    const std::array counts{
        lanes - 1u,
        lanes,
        lanes + 1u,
        2u * lanes + 1u,
    };
    for (const std::size_t count : counts) {
        const auto anchors = irregular_fixture(
            count, 0xB0A0D000u + count);
        const auto params = bounded_params();
        const auto actual = run_simd(anchors, params);

        INFO("anchor count: " << count);
        require_no_forbidden_scalar_work(actual.proof);
        require_selected_policy(
            actual.proof,
            ch::SimdChainingPolicy::bounded_batch_approximate);
        REQUIRE(actual.proof.accepted(
            ch::SimdChainingPolicy::bounded_batch_approximate));
        if (count <= 1u) {
            REQUIRE(actual.proof.policy.bounded_score_batches == 0u);
        } else {
            REQUIRE(actual.proof.policy.bounded_score_batches > 0);
        }
        REQUIRE(
            actual.proof.policy.bounded_improving_batches
                    + actual.proof.policy.bounded_nonimproving_batches
                == actual.proof.policy.bounded_score_batches);
        observed_masked_batch = observed_masked_batch
            || actual.proof.score_masked_lanes > 0;
    }
    REQUIRE(observed_masked_batch);
}

TEST_CASE(
    "bounded SIMD keeps nonzero skip penalty in full and masked batches",
    "[chaining][simd][bounded][skip_penalty][tail]")
{
    hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    REQUIRE(lanes >= 2u);

    auto params = bounded_params();
    params.max_chain_skip = 10000;
    params.chain_skip_scale = 3.25F;
    params.prepare();
    REQUIRE(params.skip_penalty > 0.0F);

    const auto anchors = irregular_fixture(
        2u * lanes + 7u, 0xB0A0D5A1u);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(
        actual, expected, "bounded nonzero skip penalty");
    require_no_forbidden_scalar_work(actual.proof);
    require_selected_policy(
        actual.proof,
        ch::SimdChainingPolicy::bounded_batch_approximate);
    REQUIRE(actual.proof.score_masked_lanes > 0u);
    REQUIRE(
        actual.proof.score_vector_lanes
        > actual.proof.score_vector_batches);
    REQUIRE(actual.proof.policy.bounded_cutoff_stops == 0u);
}

TEST_CASE(
    "bounded SIMD cutoff rescues an older active-window global best",
    "[chaining][simd][bounded][cutoff][global-best][rescue][proof]")
{
    auto params = bounded_params();
    params.bandwidth = 10000;
    params.max_gap = 10000;
    params.max_chain_skip = 25;
    const std::size_t main_count = 128u;
    const auto anchors = older_winner_fixture(main_count, 512u);
    const auto actual = run_simd(anchors, params);
    const auto aggregate = run_simd(
        anchors, params, /*aggregate_proof=*/true);

    require_no_forbidden_scalar_work(actual.proof);
    require_selected_policy(
        actual.proof,
        ch::SimdChainingPolicy::bounded_batch_approximate,
        /*require_accelerated_work=*/true);
    REQUIRE(actual.proof.accepted(
        ch::SimdChainingPolicy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/true));
    REQUIRE(actual.proof.policy.bounded_cutoff_stops > 0);
    REQUIRE(actual.proof.policy.bounded_candidates_skipped > 0);
    REQUIRE(actual.proof.policy.global_best_rescue_probes > 0);
    REQUIRE(actual.proof.policy.global_best_rescue_wins > 0);
    REQUIRE_FALSE(actual.chains.empty());
    REQUIRE(
        actual.chains.front().anchor_count
        == static_cast<std::int32_t>(main_count + 1u));
    REQUIRE(
        aggregate.proof.policy.bounded_score_batches
        == actual.proof.policy.bounded_score_batches);
    REQUIRE(
        aggregate.proof.policy.bounded_improving_batches
        == actual.proof.policy.bounded_improving_batches);
    REQUIRE(
        aggregate.proof.policy.bounded_nonimproving_batches
        == actual.proof.policy.bounded_nonimproving_batches);
    REQUIRE(
        aggregate.proof.policy.bounded_cutoff_stops
        == actual.proof.policy.bounded_cutoff_stops);
    REQUIRE(
        aggregate.proof.policy.bounded_candidates_skipped
        == actual.proof.policy.bounded_candidates_skipped);
    REQUIRE(
        aggregate.proof.policy.global_best_rescue_probes
        == actual.proof.policy.global_best_rescue_probes);
    REQUIRE(
        aggregate.proof.policy.global_best_rescue_wins
        == actual.proof.policy.global_best_rescue_wins);
    REQUIRE(aggregate.proof.accepted(
        ch::SimdChainingPolicy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/true));
}

TEST_CASE(
    "bounded SIMD keeps the newest predecessor on score ties",
    "[chaining][simd][bounded][ties][largest-index]")
{
    auto params = bounded_params();
    params.max_chain_skip = 10000;
    const auto anchors = equal_score_tie_fixture(129u);
    const auto actual = run_simd(anchors, params);

    require_no_forbidden_scalar_work(actual.proof);
    require_selected_policy(
        actual.proof,
        ch::SimdChainingPolicy::bounded_batch_approximate);
    REQUIRE_FALSE(actual.chains.empty());
    const auto& primary = actual.chains.front();
    REQUIRE(primary.anchor_count == 129);
    for (std::int32_t offset = 0;
         offset < primary.anchor_count; ++offset)
    {
        REQUIRE(
            actual.anchors[static_cast<std::size_t>(
                primary.anchor_start + offset)].flags
            == 1u);
    }
}

TEST_CASE(
    "bounded SIMD has no universal endpoint theorem",
    "[chaining][simd][bounded][theorem-counterexample]")
{
    auto exact = exact_params();
    exact.max_gap = 10000;
    exact.bandwidth = 1000;
    exact.max_chain_skip = 10000;
    auto bounded = exact;
    bounded.max_chain_skip = 25;
    bounded.simd_policy =
        ch::SimdChainingPolicy::bounded_batch_approximate;
    const auto anchors = theorem_counterexample_fixture();
    const auto scalar = run_scalar(anchors, exact);
    const auto exact_simd = run_simd(anchors, exact);
    const auto bounded_simd = run_simd(anchors, bounded);

    require_same_outputs(
        exact_simd, scalar, "theorem counterexample exact oracle");
    REQUIRE(outputs_differ(bounded_simd, exact_simd));
    REQUIRE(bounded_simd.proof.policy.bounded_cutoff_stops > 0);
    REQUIRE(bounded_simd.proof.policy.bounded_candidates_skipped > 0);
    require_no_forbidden_scalar_work(bounded_simd.proof);
}

TEST_CASE(
    "SIMD chaining proof rejects mixed selected policies",
    "[chaining][simd][policy][aggregate][mixed][fail-closed]")
{
    const auto anchors = diagonal_fixture(273u);
    const auto exact = run_simd(anchors, exact_params());
    const auto bounded = run_simd(anchors, bounded_params());
    SimdChainingExecutionProof mixed{};
    mixed.combine(exact.proof);
    mixed.combine(bounded.proof);

    REQUIRE_FALSE(mixed.accepted(
        ch::SimdChainingPolicy::exact_visited));
    REQUIRE_FALSE(mixed.accepted(
        ch::SimdChainingPolicy::bounded_batch_approximate));
    REQUIRE_FALSE(mixed.policy_evidence_coherent(
        ch::SimdChainingPolicy::exact_visited));
    REQUIRE_FALSE(mixed.policy_evidence_coherent(
        ch::SimdChainingPolicy::bounded_batch_approximate));

    auto impossible_classification = bounded.proof;
    ++impossible_classification.policy.bounded_nonimproving_batches;
    REQUIRE_FALSE(impossible_classification.policy_evidence_coherent(
        ch::SimdChainingPolicy::bounded_batch_approximate));

    auto impossible_rescue = bounded.proof;
    impossible_rescue.policy.global_best_rescue_wins =
        impossible_rescue.policy.global_best_rescue_probes + 1u;
    REQUIRE_FALSE(impossible_rescue.policy_evidence_coherent(
        ch::SimdChainingPolicy::bounded_batch_approximate));
}

TEST_CASE(
    "bounded SIMD recomputes the largest global best after window expiry",
    "[chaining][simd][bounded][window][reverse][global-best]")
{
    std::vector<ch::Anchor> anchors = diagonal_fixture(33u);
    for (std::size_t i = 0; i < 10u; ++i) {
        anchors.push_back(make_anchor(
            2000u + i * 9u,
            400u + i * 9u,
            9u,
            /*reverse=*/1u));
    }
    // These anchors cannot extend the preceding reverse-strand chain. Its
    // cached winner therefore remains old until max_chain_iter advances the
    // active window past it and forces a vector global-best recomputation.
    for (std::size_t i = 0; i < 31u; ++i) {
        anchors.push_back(make_anchor(
            2100u + i,
            100u,
            9u,
            /*reverse=*/1u));
    }
    auto params = bounded_params();
    params.max_chain_iter = 19;
    params.max_gap = 1000;
    params.bandwidth = 250;
    const auto actual = run_simd(anchors, params);

    require_no_forbidden_scalar_work(actual.proof);
    require_selected_policy(
        actual.proof,
        ch::SimdChainingPolicy::bounded_batch_approximate);
    REQUIRE(actual.proof.global_best_vector_batches > 0);
    REQUIRE_FALSE(actual.chains.empty());
}

TEST_CASE(
    "bounded SIMD rejects unsupported policy requests without substitution",
    "[chaining][simd][bounded][policy][fail-closed]")
{
    const auto anchors = diagonal_fixture(8u);
    auto invoke_rejected = [&anchors](
        ch::ChainingParams params,
        SimdChainingStatus expected_status)
    {
        context::context ctx;
        context::installed_context_scope install{ctx};
        auto chains = ch::make_arena_slab<ch::Chain>();
        auto reordered = ch::make_arena_slab<ch::Anchor>();
        chains.reserve(anchors.size() + 1u);
        reordered.reserve(anchors.size() + 1u);
        SimdChainingExecutionProof proof{};
        try {
            ch::chain_anchors_simd(
                fixed_chaining_kernel(),
                anchors,
                params,
                chains,
                reordered,
                &proof);
            FAIL("unsupported SIMD chaining policy request must throw");
        } catch (const SimdChainingRuntimeError& error) {
            REQUIRE(error.status() == expected_status);
        }
        REQUIRE(proof.fail_closed_rejections == 1u);
        REQUIRE(proof.policy.policy_rejections == 1u);
        REQUIRE(proof.scalar_score_branches == 0u);
        REQUIRE(proof.scalar_backtrack_branches == 0u);
        REQUIRE(proof.shared_scalar_backtrack_calls == 0u);
        REQUIRE(chains.size() == 0u);
        REQUIRE(reordered.size() == 0u);
    };

    SECTION("zero max skip is exact-only") {
        auto params = bounded_params();
        params.max_chain_skip = 0;
        invoke_rejected(params, SimdChainingStatus::invalid_parameters);
    }
    SECTION("unknown enum value") {
        auto params = bounded_params();
        params.simd_policy =
            static_cast<ch::SimdChainingPolicy>(255u);
        invoke_rejected(params, SimdChainingStatus::invalid_policy);
    }
}

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
        require_selected_policy(
            actual.proof,
            ch::SimdChainingPolicy::exact_visited);
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
    REQUIRE(actual.proof.backtrack_primary_contiguous_batches > 0);
    REQUIRE(
        actual.proof.backtrack_all_starts_visited_fast_batches > 0);
}

TEST_CASE(
    "exact SIMD primary backtracking masks inactive gather indices",
    "[chaining][simd][exact][backtrack][masked-gather][regression]")
{
    const hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    REQUIRE(lanes >= 2u);

    auto params = exact_params();
    params.max_chain_skip = 10000;
    const std::size_t group_count = 2u * lanes + 1u;
    const auto anchors = equal_score_tie_fixture(group_count);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(
        actual, expected, "masked primary backtrack gather");
    require_no_forbidden_scalar_work(actual.proof);
    REQUIRE_FALSE(actual.chains.empty());
    REQUIRE(
        actual.chains.front().anchor_count
        == static_cast<std::int32_t>(group_count));
    REQUIRE(actual.proof.backtrack_path_vector_batches > 1u);
    REQUIRE(actual.proof.backtrack_primary_contiguous_batches == 0u);
}

TEST_CASE(
    "SIMD backtracking keeps zero-threshold visited starts observable",
    "[chaining][simd][exact][backtrack][visited-starts][zero-threshold]")
{
    auto params = exact_params();
    params.min_chain_score = 0;
    params.min_seed_count = 0;
    const auto anchors = diagonal_fixture(53);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(
        actual, expected, "zero-threshold visited starts");
    require_no_forbidden_scalar_work(actual.proof);
    REQUIRE(actual.chains.size() > 1);
    REQUIRE(
        actual.proof.backtrack_all_starts_visited_fast_batches == 0);
}

TEST_CASE(
    "long-read exact SIMD hybrid preserves diagonal outputs and proof",
    "[chaining][simd][long-read][hybrid][execution-proof]")
{
    auto params = exact_params();
    // Keep the skip budget above this fixture's full predecessor search so the
    // hybrid route and strict scalar oracle have identical parents.
    params.max_chain_skip = 10000;
    const std::size_t count = 273u;
    const auto anchors = diagonal_fixture(count);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(actual, expected, "long-read exact hybrid");
    require_no_forbidden_scalar_work(actual.proof);
    REQUIRE(actual.proof.score_vector_batches > 0);
    REQUIRE(actual.proof.score_vector_lanes > 0);
    REQUIRE(actual.proof.descending_candidate_steps > 0);
    REQUIRE(actual.proof.visited_marks > 0);
    REQUIRE(actual.proof.contiguous_predecessor_store_batches > 0);
    REQUIRE(actual.proof.no_improvement_fast_batches > 0);
    REQUIRE(actual.proof.improvement_replay_batches > 0);
    REQUIRE(actual.proof.improvement_replay_lanes > 0);
    REQUIRE(actual.proof.backtrack_rank_batches > 0);
    REQUIRE(actual.proof.backtrack_path_vector_batches > 0);
    REQUIRE(actual.proof.backtrack_commit_vector_batches > 0);
}

TEST_CASE(
    "aggregate SIMD hybrid proof is coherent and rejects tampering",
    "[chaining][simd][hybrid][aggregate-proof][tamper]")
{
    auto params = exact_params();
    const auto anchors = diagonal_fixture(273u);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(
        anchors, params, /*aggregate_proof=*/true);
    const auto detailed = run_simd(
        anchors, params, /*aggregate_proof=*/false);

    require_same_outputs(actual, expected, "aggregate hybrid proof");
    require_same_outputs(detailed, expected, "detailed hybrid proof");
    const auto& proof = actual.proof;
    REQUIRE(proof.accepted(
        ch::SimdChainingPolicy::exact_visited,
        /*require_nonempty_vector_work=*/true));
    REQUIRE(proof.exact_hybrid_evidence_coherent());
    REQUIRE(proof.no_improvement_fast_batches > 0);
    REQUIRE(proof.improvement_replay_batches > 0);
    REQUIRE(proof.improvement_replay_lanes > 0);
    REQUIRE(proof.contiguous_predecessor_store_batches > 0);
    REQUIRE(proof.backtrack_primary_contiguous_batches > 0);
    REQUIRE(proof.backtrack_all_starts_visited_fast_batches > 0);
    REQUIRE(proof.scalar_orchestration_steps > 0);
    REQUIRE(
        proof.no_improvement_fast_batches
                + proof.improvement_replay_batches
            <= proof.score_vector_batches);
    REQUIRE(
        proof.score_vector_batches
        == detailed.proof.score_vector_batches);
    REQUIRE(
        proof.score_vector_lanes
        == detailed.proof.score_vector_lanes);
    REQUIRE(
        proof.score_masked_lanes
        == detailed.proof.score_masked_lanes);
    REQUIRE(
        proof.no_improvement_fast_batches
        == detailed.proof.no_improvement_fast_batches);
    REQUIRE(
        proof.improvement_replay_batches
        == detailed.proof.improvement_replay_batches);
    REQUIRE(
        proof.improvement_replay_lanes
        == detailed.proof.improvement_replay_lanes);
    REQUIRE(
        proof.contiguous_predecessor_store_batches
        == detailed.proof.contiguous_predecessor_store_batches);
    REQUIRE(
        proof.backtrack_primary_contiguous_batches
        == detailed.proof.backtrack_primary_contiguous_batches);
    REQUIRE(
        proof.backtrack_all_starts_visited_fast_batches
        == detailed.proof.backtrack_all_starts_visited_fast_batches);
    REQUIRE(
        proof.scalar_orchestration_steps
        == detailed.proof.scalar_orchestration_steps);

    auto missing_classification = proof;
    missing_classification.no_improvement_fast_batches = 0;
    missing_classification.improvement_replay_batches = 0;
    missing_classification.improvement_replay_lanes = 0;
    REQUIRE_FALSE(missing_classification.accepted(
        ch::SimdChainingPolicy::exact_visited, true));

    auto missing_replay_lanes = proof;
    missing_replay_lanes.improvement_replay_lanes = 0;
    REQUIRE_FALSE(missing_replay_lanes.accepted(
        ch::SimdChainingPolicy::exact_visited, true));

    auto missing_control_steps = proof;
    missing_control_steps.scalar_orchestration_steps =
        proof.improvement_replay_batches - 1u;
    REQUIRE_FALSE(missing_control_steps.accepted(
        ch::SimdChainingPolicy::exact_visited, true));

    auto impossible_batch_total = proof;
    impossible_batch_total.score_vector_batches =
        proof.no_improvement_fast_batches
        + proof.improvement_replay_batches - 1u;
    REQUIRE_FALSE(impossible_batch_total.accepted(
        ch::SimdChainingPolicy::exact_visited, true));

    SimdChainingExecutionProof combined{};
    combined.combine(proof);
    combined.combine(proof);
    REQUIRE(combined.accepted(
        ch::SimdChainingPolicy::exact_visited, true));
    REQUIRE(
        combined.improvement_replay_batches
        == 2u * proof.improvement_replay_batches);
    REQUIRE(
        combined.backtrack_primary_contiguous_batches
        == 2u * proof.backtrack_primary_contiguous_batches);
    REQUIRE(
        combined.scalar_orchestration_steps
        == 2u * proof.scalar_orchestration_steps);
}

TEST_CASE(
    "long-read exact SIMD hybrid honors production max-skip bounds",
    "[chaining][simd][long-read][hybrid][max-skip]")
{
    const std::size_t count = 273u;
    const auto anchors = diagonal_fixture(count);
    for (const std::int32_t max_skip : {0, 25}) {
        auto params = exact_params();
        params.max_chain_skip = max_skip;
        const auto expected = run_scalar(anchors, params);
        const auto actual = run_simd(anchors, params);
        require_same_outputs(
            actual,
            expected,
            "long-read max_skip=" + std::to_string(max_skip));
        require_no_forbidden_scalar_work(actual.proof);
        REQUIRE(actual.proof.score_vector_batches > 0);
        REQUIRE(actual.proof.max_chain_skip_stops > 0);
    }
}

TEST_CASE(
    "exact SIMD hybrid reaches an older winner behind unvisited losers",
    "[chaining][simd][long-read][hybrid][older-winner]")
{
    struct FixtureCase {
        std::size_t main_count;
        std::size_t distractor_count;
        std::int32_t max_skip;
    };
    constexpr std::array cases{
        FixtureCase{128u, 128u, 0},
        FixtureCase{511u, 512u, 25},
    };

    for (const auto& fixture : cases) {
        auto params = exact_params();
        params.bandwidth = 10000;
        params.max_gap = 10000;
        params.max_chain_skip = fixture.max_skip;
        const auto anchors = older_winner_fixture(
            fixture.main_count, fixture.distractor_count);
        const auto expected = run_scalar(anchors, params);
        const auto actual = run_simd(anchors, params);

        INFO("anchor count: " << anchors.size());
        INFO("max skip: " << fixture.max_skip);
        REQUIRE(expected.chains.size() > 0);
        REQUIRE(
            expected.chains.front().anchor_count
            == static_cast<std::int32_t>(fixture.main_count + 1u));
        require_same_outputs(actual, expected, "older winner wall");
        require_no_forbidden_scalar_work(actual.proof);
        REQUIRE(actual.proof.no_improvement_fast_batches > 0);
        REQUIRE(actual.proof.improvement_replay_batches > 0);
    }
}

TEST_CASE(
    "exact SIMD hybrid matches randomized long inputs and nonzero penalty",
    "[chaining][simd][long-read][hybrid][randomized][penalty]")
{
    struct FixtureCase {
        std::size_t count;
        std::uint64_t seed;
        std::int32_t max_skip;
        float skip_scale;
    };
    constexpr std::array cases{
        FixtureCase{257u, 0x31A5u, 0, 0.0F},
        FixtureCase{513u, 0xC0FFEEu, 25, 0.0F},
        FixtureCase{1024u, 0x51A7E5u, 25, 4.0F},
    };

    bool observed_fast_batch = false;
    bool observed_replay_batch = false;
    bool observed_penalty_effect = false;
    for (const auto& fixture : cases) {
        auto params = exact_params();
        params.max_chain_skip = fixture.max_skip;
        params.chain_skip_scale = fixture.skip_scale;
        params.prepare();
        const auto anchors = irregular_fixture(
            fixture.count, fixture.seed);
        const auto expected = run_scalar(anchors, params);
        const auto actual = run_simd(anchors, params);

        INFO("anchor count: " << fixture.count);
        INFO("max skip: " << fixture.max_skip);
        INFO("skip scale: " << fixture.skip_scale);
        require_same_outputs(actual, expected, "randomized exact hybrid");
        require_no_forbidden_scalar_work(actual.proof);
        observed_fast_batch = observed_fast_batch
            || actual.proof.no_improvement_fast_batches > 0;
        observed_replay_batch = observed_replay_batch
            || actual.proof.improvement_replay_batches > 0;

        if (fixture.skip_scale > 0.0F) {
            auto zero_penalty_params = params;
            zero_penalty_params.chain_skip_scale = 0.0F;
            zero_penalty_params.prepare();
            const auto zero_penalty = run_scalar(
                anchors, zero_penalty_params);
            observed_penalty_effect = outputs_differ(
                expected, zero_penalty);
        }
    }
    REQUIRE(observed_fast_batch);
    REQUIRE(observed_replay_batch);
    REQUIRE(observed_penalty_effect);
}

TEST_CASE(
    "exact SIMD hybrid preserves strict equal-score predecessor order",
    "[chaining][simd][long-read][hybrid][ties]")
{
    auto params = exact_params();
    params.max_chain_skip = 10000;
    const auto anchors = equal_score_tie_fixture(129u);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(actual, expected, "long equal-score ties");
    require_no_forbidden_scalar_work(actual.proof);
    REQUIRE(expected.chains.size() > 0);
    const auto& best_chain = expected.chains.front();
    REQUIRE(best_chain.anchor_count == 129);
    for (std::int32_t offset = 0;
         offset < best_chain.anchor_count; ++offset)
    {
        REQUIRE(
            expected.anchors[
                static_cast<std::size_t>(best_chain.anchor_start + offset)]
                .flags
            == 1u);
    }
    REQUIRE(actual.proof.improvement_replay_batches > 0);
    // Duplicate anchors make every full predecessor batch contain an invalid
    // same-position lane, so the contiguous-store predicate must reject it and
    // preserve the exact masked-scatter fallback.
    REQUIRE(actual.proof.visited_marks > 0);
    REQUIRE(actual.proof.contiguous_predecessor_store_batches == 0);
    REQUIRE(actual.proof.backtrack_primary_contiguous_batches == 0);
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
        ch::chain_anchors_simd(
            fixed_chaining_kernel(),
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
