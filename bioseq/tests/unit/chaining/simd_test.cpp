/**
 * @file
 * @brief Verifies fixed-target SIMD chaining policies against scalar oracles.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <hwy/highway.h>
#include <span>
#include <stdexcept>
#include <string>
#include <tests/support/common.hpp>
#include <vector>

import bioseq.common.context;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.core;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace {

namespace ch = ::bioseq::chaining;
using ch::simd_chaining_execution_stats;
using ch::simd_chaining_runtime_error;
using ch::simd_chaining_status;

/**
 * @brief Returns the generated fixed-target chaining entry used by Highway tests.
 * @return Callable bound to the selected fixed target.
 * @throws std::logic_error If the generated table lacks SIMD chaining.
 */
[[nodiscard]] ch::chain_anchors_simd_kernel fixed_chaining_kernel() {
    const auto kernel =
        ::bioseq::simd_dispatch::highway_test_kernel_table().chaining.chain_anchors_simd;
    if (kernel == nullptr) {
        throw std::logic_error{"generated fixed-target test table lacks SIMD chaining"};
    }
    return kernel;
}

TEST_CASE("ArenaSlab geometric growth is amortized and chain scopes release heap fallbacks",
          "[component:bioseq][layer:unit][feature:chaining]") {
    context::context ctx;
    context::installed_context_scope installed{ctx};

    REQUIRE(ctx.chain_heap_fallback_bytes() == 0);
    {
        auto chain_scope = ctx.push<context::arena::chain>();
        auto slab = ch::make_arena_slab<std::uint64_t>();

        slab.reserve(140'000); // 1.12 MiB: force the oversize heap path.
        slab.resize(140'000);
        const auto first_capacity = slab.capacity();
        REQUIRE(ctx.chain_heap_fallback_bytes() == first_capacity * sizeof(std::uint64_t));

        slab.reserve_geometric(first_capacity + 1);
        REQUIRE(slab.capacity() == first_capacity * 2);
        REQUIRE(ctx.chain_heap_fallback_bytes() ==
                (first_capacity + slab.capacity()) * sizeof(std::uint64_t));

        // Further append-sized requests within the doubled capacity must not
        // allocate or copy another prefix.
        const auto allocated_bytes = ctx.chain_heap_fallback_bytes();
        slab.reserve_geometric(first_capacity + 1024);
        REQUIRE(ctx.chain_heap_fallback_bytes() == allocated_bytes);
    }
    REQUIRE(ctx.chain_heap_fallback_bytes() == 0);
}

/// Construct one canonical chaining anchor for fixture generation.
[[nodiscard]] ch::anchor make_anchor(std::uint64_t reference_position,
                                     std::uint64_t query_position,
                                     std::uint8_t span = 15,
                                     std::uint32_t reverse = 0) {
    return ch::anchor{
        .ref_position = reference_position,
        .query_position = query_position,
        .is_reverse = reverse,
        .query_span = span,
        .flags = 0,
        .segment_id = 0,
    };
}

/// Generate a deterministic irregular diagonal anchor stream.
[[nodiscard]] std::vector<ch::anchor> irregular_fixture(std::size_t count,
                                                        std::uint64_t seed = 0xA57D1E5ULL) {
    std::vector<ch::anchor> anchors;
    anchors.reserve(count);
    std::uint64_t ref = (std::uint64_t{1} << 32) - 23;
    std::uint64_t query = 100;
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < count; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        ref += 5 + ((state >> 9) % 19);
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        query += 4 + ((state >> 13) % 17);
        anchors.push_back(
            make_anchor(ref, query, static_cast<std::uint8_t>(7 + ((state >> 21) % 14))));
    }
    return anchors;
}

/// Generate a uniformly spaced diagonal anchor stream.
[[nodiscard]] std::vector<ch::anchor> diagonal_fixture(std::size_t count) {
    std::vector<ch::anchor> anchors;
    anchors.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        anchors.push_back(make_anchor(1000 + i * 10, 200 + i * 10, 10));
    }
    return anchors;
}

/// Generate an earlier exact winner followed by off-diagonal distractors.
[[nodiscard]] std::vector<ch::anchor> earlier_winner_fixture(std::size_t main_count,
                                                             std::size_t distractor_count) {
    constexpr std::uint64_t current_ref = 10000;
    constexpr std::uint64_t current_query = 10000;
    constexpr std::size_t distractor_positions = 32u;
    std::vector<ch::anchor> anchors;
    anchors.reserve(main_count + distractor_count + 1u);

    const std::uint64_t main_ref_end = current_ref - 100u;
    const std::uint64_t main_query_end = current_query - 100u;
    for (std::size_t i = 0; i < main_count; ++i) {
        const std::uint64_t distance = static_cast<std::uint64_t>(main_count - i - 1u) * 10u;
        anchors.push_back(make_anchor(main_ref_end - distance, main_query_end - distance, 1));
    }

    const std::size_t repetitions = distractor_count / distractor_positions;
    for (std::size_t position = 0; position < distractor_positions; ++position) {
        for (std::size_t repeat = 0; repeat < repetitions; ++repeat) {
            anchors.push_back(make_anchor(
                current_ref - distractor_positions + position, current_query - 9000u, 1));
        }
    }
    anchors.push_back(make_anchor(current_ref, current_query, 64));
    return anchors;
}

/// Generate duplicate anchors that exercise equal-score predecessor ties.
[[nodiscard]] std::vector<ch::anchor> equal_score_tie_fixture(std::size_t group_count) {
    std::vector<ch::anchor> anchors;
    anchors.reserve(group_count * 2u);
    for (std::size_t group = 0; group < group_count; ++group) {
        for (std::uint8_t duplicate = 0; duplicate < 2u; ++duplicate) {
            auto anchor = make_anchor(1000u + group * 8u, 1000u + group * 8u, 8);
            anchor.flags = duplicate;
            anchors.push_back(anchor);
        }
    }
    return anchors;
}

/// Return permissive exact-SIMD chaining parameters for fixtures.
[[nodiscard]] ch::chaining_parameters exact_params() {
    ch::chaining_parameters params;
    params.max_gap = 1000;
    params.bandwidth = 250;
    params.max_chain_skip = 10000;
    params.max_chain_iter = 10000;
    params.min_seed_count = 1;
    params.min_chain_score = 0;
    params.chain_gap_scale = 0.8F;
    params.chain_skip_scale = 0.0F;
    params.simd_policy = ch::simd_chaining_policy::exact_visited;
    params.prepare();
    return params;
}

/// Return bounded-batch chaining parameters derived from the exact fixture.
[[nodiscard]] ch::chaining_parameters bounded_params() {
    auto params = exact_params();
    params.max_chain_skip = 25;
    params.simd_policy = ch::simd_chaining_policy::bounded_batch_approximate;
    return params;
}

/// An earlier exact winner that is not the active-window global-best score.
/// The intervening off-diagonal chain can be extended arbitrarily, so a
/// bounded batch cutoff has no universal endpoint-error bound. This is a
/// counterexample fixture, not endpoint-qualification corpus data.
[[nodiscard]] std::vector<ch::anchor> unbounded_endpoint_error_fixture() {
    std::vector<ch::anchor> anchors;
    anchors.reserve(221u);

    // Exact winner: score 100, but a 64-span final anchor contributes 64 to
    // the last current-anchor edge and makes the resulting chain score 164.
    for (std::size_t i = 0; i < 100u; ++i) {
        anchors.push_back(make_anchor(7000u + i * 10u, 7000u + i * 10u, i == 99u ? 64u : 1u));
    }

    // Active-window global best: score 120. It is strongly off diagonal from
    // the final anchor, so the rescue probe cannot use it.
    for (std::size_t i = 0; i < 120u; ++i) {
        anchors.push_back(make_anchor(8500u + i * 10u, 1000u + i * 10u, 1u));
    }
    anchors.push_back(make_anchor(10000u, 10000u, 1u));
    return anchors;
}

/// Owned chains, reordered anchors, and execution stats from one fixture run.
struct chain_outputs {
    std::vector<ch::chain> chains;
    std::vector<ch::anchor> anchors;
    simd_chaining_execution_stats stats{};
};

/// Run the scalar correctness oracle inside a fresh chaining arena.
[[nodiscard]] chain_outputs run_scalar(std::span<const ch::anchor> anchors,
                                       const ch::chaining_parameters& params) {
    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains = ch::make_arena_slab<ch::chain>();
    auto reordered = ch::make_arena_slab<ch::anchor>();
    chains.reserve(anchors.size() + 1);
    reordered.reserve(anchors.size() + 1);
    ch::chain_anchors_scalar(anchors, params, chains, reordered);
    chain_outputs result;
    result.chains.assign(chains.data(), chains.data() + chains.size());
    result.anchors.assign(reordered.data(), reordered.data() + reordered.size());
    return result;
}

/// Run the fixed-target SIMD route inside a fresh chaining arena.
[[nodiscard]] chain_outputs run_simd(std::span<const ch::anchor> anchors,
                                     const ch::chaining_parameters& params,
                                     bool aggregate_stats = false) {
    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains = ch::make_arena_slab<ch::chain>();
    auto reordered = ch::make_arena_slab<ch::anchor>();
    chains.reserve(anchors.size() + 1);
    reordered.reserve(anchors.size() + 1);
    chain_outputs result;
    ch::chain_anchors_simd(fixed_chaining_kernel(),
                           anchors,
                           params,
                           chains,
                           reordered,
                           &result.stats,
                           aggregate_stats);
    result.chains.assign(chains.data(), chains.data() + chains.size());
    result.anchors.assign(reordered.data(), reordered.data() + reordered.size());
    return result;
}

/// Require exact chain and reordered-anchor parity between two runs.
void require_same_outputs(const chain_outputs& actual,
                          const chain_outputs& expected,
                          const std::string& fixture) {
    INFO("fixture: " << fixture);
    REQUIRE(actual.chains.size() == expected.chains.size());
    REQUIRE(actual.anchors.size() == expected.anchors.size());
    for (std::size_t i = 0; i < expected.chains.size(); ++i) {
        INFO("chain index: " << i);
        REQUIRE(actual.chains[i].score == expected.chains[i].score);
        REQUIRE(actual.chains[i].anchor_count == expected.chains[i].anchor_count);
        REQUIRE(actual.chains[i].anchor_start == expected.chains[i].anchor_start);
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

/// Require that a selected SIMD route did not execute scalar fallback work.
void require_no_forbidden_scalar_work(const simd_chaining_execution_stats& stats) {
    REQUIRE(stats.scalar_score_branches == 0);
    REQUIRE(stats.scalar_backtrack_branches == 0);
    REQUIRE(stats.shared_scalar_backtrack_calls == 0);
    REQUIRE(stats.fail_closed_rejections == 0);
}

/// Require coherent execution statistics for the selected SIMD policy.
void require_selected_policy(const simd_chaining_execution_stats& stats,
                             ch::simd_chaining_policy selected,
                             bool require_accelerated_work = false) {
    REQUIRE(stats.has_consistent_policy_stats(selected, require_accelerated_work));
    switch (selected) {
    case ch::simd_chaining_policy::exact_visited:
        REQUIRE(stats.policy.exact_visited_calls == 1);
        REQUIRE(stats.policy.bounded_batch_approximate_calls == 0);
        break;
    case ch::simd_chaining_policy::bounded_batch_approximate:
        REQUIRE(stats.policy.exact_visited_calls == 0);
        REQUIRE(stats.policy.bounded_batch_approximate_calls == 1);
        break;
    }
}

/// Return whether two runs differ in chain or reordered-anchor identity.
[[nodiscard]] bool outputs_differ(const chain_outputs& lhs, const chain_outputs& rhs) {
    if (lhs.chains.size() != rhs.chains.size() || lhs.anchors.size() != rhs.anchors.size()) {
        return true;
    }
    for (std::size_t i = 0; i < lhs.chains.size(); ++i) {
        if (lhs.chains[i].score != rhs.chains[i].score ||
            lhs.chains[i].anchor_count != rhs.chains[i].anchor_count ||
            lhs.chains[i].anchor_start != rhs.chains[i].anchor_start) {
            return true;
        }
    }
    for (std::size_t i = 0; i < lhs.anchors.size(); ++i) {
        if (lhs.anchors[i].ref_position != rhs.anchors[i].ref_position ||
            lhs.anchors[i].query_position != rhs.anchors[i].query_position ||
            lhs.anchors[i].flags != rhs.anchors[i].flags) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("SIMD chaining policy defaults exact and has stable names",
          "[component:bioseq][layer:unit][feature:chaining]") {
    const ch::chaining_parameters params{};
    REQUIRE(params.simd_policy == ch::simd_chaining_policy::exact_visited);
    REQUIRE(ch::simd_chaining_policy_name(ch::simd_chaining_policy::exact_visited) ==
            "exact_visited");
    REQUIRE(ch::simd_chaining_policy_name(ch::simd_chaining_policy::bounded_batch_approximate) ==
            "bounded_batch_approximate");
    REQUIRE(ch::simd_chaining_policy_name(static_cast<ch::simd_chaining_policy>(255u)) ==
            "unknown");
}

TEST_CASE("bounded SIMD chaining uses Highway for full and masked batches",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
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
        const auto anchors = irregular_fixture(count, 0xB0A0D000u + count);
        const auto params = bounded_params();
        const auto actual = run_simd(anchors, params);

        INFO("anchor count: " << count);
        require_no_forbidden_scalar_work(actual.stats);
        require_selected_policy(actual.stats, ch::simd_chaining_policy::bounded_batch_approximate);
        REQUIRE(actual.stats.is_internally_consistent(
            ch::simd_chaining_policy::bounded_batch_approximate));
        if (count <= 1u) {
            REQUIRE(actual.stats.policy.bounded_score_batches == 0u);
        } else {
            REQUIRE(actual.stats.policy.bounded_score_batches > 0);
        }
        REQUIRE(actual.stats.policy.bounded_improving_batches +
                    actual.stats.policy.bounded_nonimproving_batches ==
                actual.stats.policy.bounded_score_batches);
        observed_masked_batch = observed_masked_batch || actual.stats.score_masked_lanes > 0;
    }
    REQUIRE(observed_masked_batch);
}

TEST_CASE("bounded SIMD keeps nonzero skip penalty in full and masked batches",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    REQUIRE(lanes >= 2u);

    auto params = bounded_params();
    params.max_chain_skip = 10000;
    params.chain_skip_scale = 3.25F;
    params.prepare();
    REQUIRE(params.skip_penalty > 0.0F);

    const auto anchors = irregular_fixture(2u * lanes + 7u, 0xB0A0D5A1u);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(actual, expected, "bounded nonzero skip penalty");
    require_no_forbidden_scalar_work(actual.stats);
    require_selected_policy(actual.stats, ch::simd_chaining_policy::bounded_batch_approximate);
    REQUIRE(actual.stats.score_masked_lanes > 0u);
    REQUIRE(actual.stats.score_vector_lanes > actual.stats.score_vector_batches);
    REQUIRE(actual.stats.policy.bounded_cutoff_stops == 0u);
}

TEST_CASE("bounded SIMD cutoff rescues an earlier active-window global best",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = bounded_params();
    params.bandwidth = 10000;
    params.max_gap = 10000;
    params.max_chain_skip = 25;
    const std::size_t main_count = 128u;
    const auto anchors = earlier_winner_fixture(main_count, 512u);
    const auto actual = run_simd(anchors, params);
    const auto aggregate = run_simd(anchors, params, /*aggregate_stats=*/true);

    require_no_forbidden_scalar_work(actual.stats);
    require_selected_policy(actual.stats,
                            ch::simd_chaining_policy::bounded_batch_approximate,
                            /*require_accelerated_work=*/true);
    REQUIRE(
        actual.stats.is_internally_consistent(ch::simd_chaining_policy::bounded_batch_approximate,
                                              /*require_nonempty_vector_work=*/true));
    REQUIRE(actual.stats.policy.bounded_cutoff_stops > 0);
    REQUIRE(actual.stats.policy.bounded_candidates_skipped > 0);
    REQUIRE(actual.stats.policy.global_best_rescue_probes > 0);
    REQUIRE(actual.stats.policy.global_best_rescue_wins > 0);
    REQUIRE_FALSE(actual.chains.empty());
    REQUIRE(actual.chains.front().anchor_count == static_cast<std::int32_t>(main_count + 1u));
    REQUIRE(aggregate.stats.policy.bounded_score_batches ==
            actual.stats.policy.bounded_score_batches);
    REQUIRE(aggregate.stats.policy.bounded_improving_batches ==
            actual.stats.policy.bounded_improving_batches);
    REQUIRE(aggregate.stats.policy.bounded_nonimproving_batches ==
            actual.stats.policy.bounded_nonimproving_batches);
    REQUIRE(aggregate.stats.policy.bounded_cutoff_stops ==
            actual.stats.policy.bounded_cutoff_stops);
    REQUIRE(aggregate.stats.policy.bounded_candidates_skipped ==
            actual.stats.policy.bounded_candidates_skipped);
    REQUIRE(aggregate.stats.policy.global_best_rescue_probes ==
            actual.stats.policy.global_best_rescue_probes);
    REQUIRE(aggregate.stats.policy.global_best_rescue_wins ==
            actual.stats.policy.global_best_rescue_wins);
    REQUIRE(aggregate.stats.is_internally_consistent(
        ch::simd_chaining_policy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/true));
}

TEST_CASE("bounded SIMD keeps the newest predecessor on score ties",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = bounded_params();
    params.max_chain_skip = 10000;
    const auto anchors = equal_score_tie_fixture(129u);
    const auto actual = run_simd(anchors, params);

    require_no_forbidden_scalar_work(actual.stats);
    require_selected_policy(actual.stats, ch::simd_chaining_policy::bounded_batch_approximate);
    REQUIRE_FALSE(actual.chains.empty());
    const auto& primary = actual.chains.front();
    REQUIRE(primary.anchor_count == 129);
    for (std::int32_t offset = 0; offset < primary.anchor_count; ++offset) {
        REQUIRE(actual.anchors[static_cast<std::size_t>(primary.anchor_start + offset)].flags ==
                1u);
    }
}

TEST_CASE("bounded SIMD has no universal endpoint-error bound",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto exact = exact_params();
    exact.max_gap = 10000;
    exact.bandwidth = 1000;
    exact.max_chain_skip = 10000;
    auto bounded = exact;
    bounded.max_chain_skip = 25;
    bounded.simd_policy = ch::simd_chaining_policy::bounded_batch_approximate;
    const auto anchors = unbounded_endpoint_error_fixture();
    const auto scalar = run_scalar(anchors, exact);
    const auto exact_simd = run_simd(anchors, exact);
    const auto bounded_simd = run_simd(anchors, bounded);

    require_same_outputs(exact_simd, scalar, "unbounded endpoint-error exact oracle");
    REQUIRE(outputs_differ(bounded_simd, exact_simd));
    REQUIRE(bounded_simd.stats.policy.bounded_cutoff_stops > 0);
    REQUIRE(bounded_simd.stats.policy.bounded_candidates_skipped > 0);
    require_no_forbidden_scalar_work(bounded_simd.stats);
}

TEST_CASE("SIMD chaining stats rejects mixed selected policies",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    const auto anchors = diagonal_fixture(273u);
    const auto exact = run_simd(anchors, exact_params());
    const auto bounded = run_simd(anchors, bounded_params());
    simd_chaining_execution_stats mixed{};
    mixed.merge(exact.stats);
    mixed.merge(bounded.stats);

    REQUIRE_FALSE(mixed.is_internally_consistent(ch::simd_chaining_policy::exact_visited));
    REQUIRE_FALSE(
        mixed.is_internally_consistent(ch::simd_chaining_policy::bounded_batch_approximate));
    REQUIRE_FALSE(mixed.has_consistent_policy_stats(ch::simd_chaining_policy::exact_visited));
    REQUIRE_FALSE(
        mixed.has_consistent_policy_stats(ch::simd_chaining_policy::bounded_batch_approximate));

    auto impossible_classification = bounded.stats;
    ++impossible_classification.policy.bounded_nonimproving_batches;
    REQUIRE_FALSE(impossible_classification.has_consistent_policy_stats(
        ch::simd_chaining_policy::bounded_batch_approximate));

    auto impossible_rescue = bounded.stats;
    impossible_rescue.policy.global_best_rescue_wins =
        impossible_rescue.policy.global_best_rescue_probes + 1u;
    REQUIRE_FALSE(impossible_rescue.has_consistent_policy_stats(
        ch::simd_chaining_policy::bounded_batch_approximate));
}

TEST_CASE("bounded SIMD recomputes the largest global best after window expiry",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    std::vector<ch::anchor> anchors = diagonal_fixture(33u);
    for (std::size_t i = 0; i < 10u; ++i) {
        anchors.push_back(make_anchor(2000u + i * 9u,
                                      400u + i * 9u,
                                      9u,
                                      /*reverse=*/1u));
    }
    // These anchors cannot extend the preceding reverse-strand chain. Its
    // cached winner therefore remains unchanged until max_chain_iter advances the
    // active window past it and forces a vector global-best recomputation.
    for (std::size_t i = 0; i < 31u; ++i) {
        anchors.push_back(make_anchor(2100u + i,
                                      100u,
                                      9u,
                                      /*reverse=*/1u));
    }
    auto params = bounded_params();
    params.max_chain_iter = 19;
    params.max_gap = 1000;
    params.bandwidth = 250;
    const auto actual = run_simd(anchors, params);

    require_no_forbidden_scalar_work(actual.stats);
    require_selected_policy(actual.stats, ch::simd_chaining_policy::bounded_batch_approximate);
    REQUIRE(actual.stats.global_best_vector_batches > 0);
    REQUIRE_FALSE(actual.chains.empty());
}

TEST_CASE("bounded SIMD rejects unsupported policy requests without substitution",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    const auto anchors = diagonal_fixture(8u);
    auto invoke_rejected = [&anchors](ch::chaining_parameters params,
                                      simd_chaining_status expected_status) {
        context::context ctx;
        context::installed_context_scope install{ctx};
        auto chains = ch::make_arena_slab<ch::chain>();
        auto reordered = ch::make_arena_slab<ch::anchor>();
        chains.reserve(anchors.size() + 1u);
        reordered.reserve(anchors.size() + 1u);
        simd_chaining_execution_stats stats{};
        try {
            ch::chain_anchors_simd(
                fixed_chaining_kernel(), anchors, params, chains, reordered, &stats);
            FAIL("unsupported SIMD chaining policy request must throw");
        } catch (const simd_chaining_runtime_error& error) {
            REQUIRE(error.status() == expected_status);
        }
        REQUIRE(stats.fail_closed_rejections == 1u);
        REQUIRE(stats.policy.policy_rejections == 1u);
        REQUIRE(stats.scalar_score_branches == 0u);
        REQUIRE(stats.scalar_backtrack_branches == 0u);
        REQUIRE(stats.shared_scalar_backtrack_calls == 0u);
        REQUIRE(chains.size() == 0u);
        REQUIRE(reordered.size() == 0u);
    };

    SECTION("zero max skip is exact-only") {
        auto params = bounded_params();
        params.max_chain_skip = 0;
        invoke_rejected(params, simd_chaining_status::invalid_parameters);
    }
    SECTION("unknown enum value") {
        auto params = bounded_params();
        params.simd_policy = static_cast<ch::simd_chaining_policy>(255u);
        invoke_rejected(params, simd_chaining_status::invalid_policy);
    }
}

TEST_CASE("exact SIMD chaining scores every small range and masked tail",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
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
        require_same_outputs(actual, expected, "small/tail n=" + std::to_string(count));
        require_no_forbidden_scalar_work(actual.stats);
        require_selected_policy(actual.stats, ch::simd_chaining_policy::exact_visited);
        REQUIRE(actual.stats.anchors_consumed == count);
        if (count > 1) {
            REQUIRE(actual.stats.score_vector_batches > 0);
            REQUIRE(actual.stats.score_vector_lanes > 0);
            REQUIRE(actual.stats.descending_candidate_steps > 0);
        }
        observed_masked_batch = observed_masked_batch || actual.stats.score_masked_lanes > 0;
    }
    REQUIRE(observed_masked_batch);
}

TEST_CASE("exact SIMD chaining includes nonzero skip penalty",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    params.chain_skip_scale = 3.25F;
    params.prepare();
    REQUIRE(params.skip_penalty > 0.0F);
    const auto anchors = irregular_fixture(37, 0x5A17);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "nonzero skip penalty");
    require_no_forbidden_scalar_work(actual.stats);
    REQUIRE(actual.stats.score_vector_batches > 0);
}

TEST_CASE("exact SIMD chaining preserves descending visited max-skip stop",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    params.max_chain_skip = 0;
    const auto anchors = diagonal_fixture(41);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "visited stop max_skip=0");
    require_no_forbidden_scalar_work(actual.stats);
    REQUIRE(actual.stats.visited_marks > 0);
    REQUIRE(actual.stats.max_chain_skip_stops > 0);
}

TEST_CASE("exact SIMD chaining preserves ties parents and output order",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    params.max_chain_skip = 10000;
    const auto anchors = diagonal_fixture(29);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "equal-score predecessor ties");
    require_no_forbidden_scalar_work(actual.stats);

    REQUIRE(actual.chains.size() == 1);
    REQUIRE(actual.chains.front().anchor_count == static_cast<std::int32_t>(anchors.size()));
    REQUIRE(actual.anchors.size() == anchors.size());
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        // The strict-greater predecessor policy keeps the nearest descending
        // predecessor on ties, yielding the complete parent chain in order.
        REQUIRE(actual.anchors[i].ref_position == anchors[i].ref_position);
        REQUIRE(actual.anchors[i].query_position == anchors[i].query_position);
    }
}

TEST_CASE("lane-batched SIMD backtracking resolves overlapping paths exactly",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    params.min_chain_score = 0;
    params.min_seed_count = 1;
    const auto anchors = diagonal_fixture(53);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);
    require_same_outputs(actual, expected, "overlapping ranked paths");
    require_no_forbidden_scalar_work(actual.stats);
    REQUIRE(actual.stats.backtrack_rank_batches > 0);
    REQUIRE(actual.stats.backtrack_path_vector_batches > 0);
    REQUIRE(actual.stats.backtrack_commit_vector_batches > 0);
    REQUIRE(actual.stats.backtrack_vector_lanes > 0);
    REQUIRE(actual.stats.backtrack_overlap_stops > 0);
    REQUIRE(actual.stats.backtrack_primary_contiguous_batches > 0);
    REQUIRE(actual.stats.backtrack_all_starts_visited_fast_batches > 0);
}

TEST_CASE("exact SIMD primary backtracking masks inactive gather indices",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    const hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    REQUIRE(lanes >= 2u);

    auto params = exact_params();
    params.max_chain_skip = 10000;
    const std::size_t group_count = 2u * lanes + 1u;
    const auto anchors = equal_score_tie_fixture(group_count);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(actual, expected, "masked primary backtrack gather");
    require_no_forbidden_scalar_work(actual.stats);
    REQUIRE_FALSE(actual.chains.empty());
    REQUIRE(actual.chains.front().anchor_count == static_cast<std::int32_t>(group_count));
    REQUIRE(actual.stats.backtrack_path_vector_batches > 1u);
    REQUIRE(actual.stats.backtrack_primary_contiguous_batches == 0u);
}

TEST_CASE("SIMD backtracking keeps zero-threshold visited starts observable",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    params.min_chain_score = 0;
    params.min_seed_count = 0;
    const auto anchors = diagonal_fixture(53);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(actual, expected, "zero-threshold visited starts");
    require_no_forbidden_scalar_work(actual.stats);
    REQUIRE(actual.chains.size() > 1);
    REQUIRE(actual.stats.backtrack_all_starts_visited_fast_batches == 0);
}

TEST_CASE("long-read exact SIMD hybrid preserves diagonal outputs and stats",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    // Keep the skip budget above this fixture's full predecessor search so the
    // hybrid route and strict scalar oracle have identical parents.
    params.max_chain_skip = 10000;
    const std::size_t count = 273u;
    const auto anchors = diagonal_fixture(count);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(actual, expected, "long-read exact hybrid");
    require_no_forbidden_scalar_work(actual.stats);
    REQUIRE(actual.stats.score_vector_batches > 0);
    REQUIRE(actual.stats.score_vector_lanes > 0);
    REQUIRE(actual.stats.descending_candidate_steps > 0);
    REQUIRE(actual.stats.visited_marks > 0);
    REQUIRE(actual.stats.contiguous_predecessor_store_batches > 0);
    REQUIRE(actual.stats.no_improvement_fast_batches > 0);
    REQUIRE(actual.stats.improvement_replay_batches > 0);
    REQUIRE(actual.stats.improvement_replay_lanes > 0);
    REQUIRE(actual.stats.backtrack_rank_batches > 0);
    REQUIRE(actual.stats.backtrack_path_vector_batches > 0);
    REQUIRE(actual.stats.backtrack_commit_vector_batches > 0);
}

TEST_CASE("aggregate SIMD hybrid stats is coherent and rejects tampering",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    const auto anchors = diagonal_fixture(273u);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params, /*aggregate_stats=*/true);
    const auto detailed = run_simd(anchors, params, /*aggregate_stats=*/false);

    require_same_outputs(actual, expected, "aggregate hybrid stats");
    require_same_outputs(detailed, expected, "detailed hybrid stats");
    const auto& stats = actual.stats;
    REQUIRE(stats.is_internally_consistent(ch::simd_chaining_policy::exact_visited,
                                           /*require_nonempty_vector_work=*/true));
    REQUIRE(stats.has_consistent_exact_execution_stats());
    REQUIRE(stats.no_improvement_fast_batches > 0);
    REQUIRE(stats.improvement_replay_batches > 0);
    REQUIRE(stats.improvement_replay_lanes > 0);
    REQUIRE(stats.contiguous_predecessor_store_batches > 0);
    REQUIRE(stats.backtrack_primary_contiguous_batches > 0);
    REQUIRE(stats.backtrack_all_starts_visited_fast_batches > 0);
    REQUIRE(stats.scalar_orchestration_steps > 0);
    REQUIRE(stats.no_improvement_fast_batches + stats.improvement_replay_batches <=
            stats.score_vector_batches);
    REQUIRE(stats.score_vector_batches == detailed.stats.score_vector_batches);
    REQUIRE(stats.score_vector_lanes == detailed.stats.score_vector_lanes);
    REQUIRE(stats.score_masked_lanes == detailed.stats.score_masked_lanes);
    REQUIRE(stats.no_improvement_fast_batches == detailed.stats.no_improvement_fast_batches);
    REQUIRE(stats.improvement_replay_batches == detailed.stats.improvement_replay_batches);
    REQUIRE(stats.improvement_replay_lanes == detailed.stats.improvement_replay_lanes);
    REQUIRE(stats.contiguous_predecessor_store_batches ==
            detailed.stats.contiguous_predecessor_store_batches);
    REQUIRE(stats.backtrack_primary_contiguous_batches ==
            detailed.stats.backtrack_primary_contiguous_batches);
    REQUIRE(stats.backtrack_all_starts_visited_fast_batches ==
            detailed.stats.backtrack_all_starts_visited_fast_batches);
    REQUIRE(stats.scalar_orchestration_steps == detailed.stats.scalar_orchestration_steps);

    auto missing_classification = stats;
    missing_classification.no_improvement_fast_batches = 0;
    missing_classification.improvement_replay_batches = 0;
    missing_classification.improvement_replay_lanes = 0;
    REQUIRE_FALSE(missing_classification.is_internally_consistent(
        ch::simd_chaining_policy::exact_visited, true));

    auto missing_replay_lanes = stats;
    missing_replay_lanes.improvement_replay_lanes = 0;
    REQUIRE_FALSE(missing_replay_lanes.is_internally_consistent(
        ch::simd_chaining_policy::exact_visited, true));

    auto missing_control_steps = stats;
    missing_control_steps.scalar_orchestration_steps = stats.improvement_replay_batches - 1u;
    REQUIRE_FALSE(missing_control_steps.is_internally_consistent(
        ch::simd_chaining_policy::exact_visited, true));

    auto impossible_batch_total = stats;
    impossible_batch_total.score_vector_batches =
        stats.no_improvement_fast_batches + stats.improvement_replay_batches - 1u;
    REQUIRE_FALSE(impossible_batch_total.is_internally_consistent(
        ch::simd_chaining_policy::exact_visited, true));

    simd_chaining_execution_stats combined{};
    combined.merge(stats);
    combined.merge(stats);
    REQUIRE(combined.is_internally_consistent(ch::simd_chaining_policy::exact_visited, true));
    REQUIRE(combined.improvement_replay_batches == 2u * stats.improvement_replay_batches);
    REQUIRE(combined.backtrack_primary_contiguous_batches ==
            2u * stats.backtrack_primary_contiguous_batches);
    REQUIRE(combined.scalar_orchestration_steps == 2u * stats.scalar_orchestration_steps);
}

TEST_CASE("long-read exact SIMD hybrid honors production max-skip bounds",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    const std::size_t count = 273u;
    const auto anchors = diagonal_fixture(count);
    for (const std::int32_t max_skip : {0, 25}) {
        auto params = exact_params();
        params.max_chain_skip = max_skip;
        const auto expected = run_scalar(anchors, params);
        const auto actual = run_simd(anchors, params);
        require_same_outputs(actual, expected, "long-read max_skip=" + std::to_string(max_skip));
        require_no_forbidden_scalar_work(actual.stats);
        REQUIRE(actual.stats.score_vector_batches > 0);
        REQUIRE(actual.stats.max_chain_skip_stops > 0);
    }
}

TEST_CASE("exact SIMD hybrid reaches an earlier winner behind unvisited losers",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    /// Exact-winner fixture dimensions and predecessor skip policy.
    struct fixture_case {
        std::size_t main_count;
        std::size_t distractor_count;
        std::int32_t max_skip;
    };
    constexpr std::array cases{
        fixture_case{128u, 128u, 0},
        fixture_case{511u, 512u, 25},
    };

    for (const auto& fixture : cases) {
        auto params = exact_params();
        params.bandwidth = 10000;
        params.max_gap = 10000;
        params.max_chain_skip = fixture.max_skip;
        const auto anchors = earlier_winner_fixture(fixture.main_count, fixture.distractor_count);
        const auto expected = run_scalar(anchors, params);
        const auto actual = run_simd(anchors, params);

        INFO("anchor count: " << anchors.size());
        INFO("max skip: " << fixture.max_skip);
        REQUIRE(expected.chains.size() > 0);
        REQUIRE(expected.chains.front().anchor_count ==
                static_cast<std::int32_t>(fixture.main_count + 1u));
        require_same_outputs(actual, expected, "earlier winner wall");
        require_no_forbidden_scalar_work(actual.stats);
        REQUIRE(actual.stats.no_improvement_fast_batches > 0);
        REQUIRE(actual.stats.improvement_replay_batches > 0);
    }
}

TEST_CASE("exact SIMD hybrid matches randomized long inputs and nonzero penalty",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    /// Randomized fixture dimensions, seed, and skip penalty configuration.
    struct fixture_case {
        std::size_t count;
        std::uint64_t seed;
        std::int32_t max_skip;
        float skip_scale;
    };
    constexpr std::array cases{
        fixture_case{257u, 0x31A5u, 0, 0.0F},
        fixture_case{513u, 0xC0FFEEu, 25, 0.0F},
        fixture_case{1024u, 0x51A7E5u, 25, 4.0F},
    };

    bool observed_fast_batch = false;
    bool observed_replay_batch = false;
    bool observed_penalty_effect = false;
    for (const auto& fixture : cases) {
        auto params = exact_params();
        params.max_chain_skip = fixture.max_skip;
        params.chain_skip_scale = fixture.skip_scale;
        params.prepare();
        const auto anchors = irregular_fixture(fixture.count, fixture.seed);
        const auto expected = run_scalar(anchors, params);
        const auto actual = run_simd(anchors, params);

        INFO("anchor count: " << fixture.count);
        INFO("max skip: " << fixture.max_skip);
        INFO("skip scale: " << fixture.skip_scale);
        require_same_outputs(actual, expected, "randomized exact hybrid");
        require_no_forbidden_scalar_work(actual.stats);
        observed_fast_batch = observed_fast_batch || actual.stats.no_improvement_fast_batches > 0;
        observed_replay_batch =
            observed_replay_batch || actual.stats.improvement_replay_batches > 0;

        if (fixture.skip_scale > 0.0F) {
            auto zero_penalty_params = params;
            zero_penalty_params.chain_skip_scale = 0.0F;
            zero_penalty_params.prepare();
            const auto zero_penalty = run_scalar(anchors, zero_penalty_params);
            observed_penalty_effect = outputs_differ(expected, zero_penalty);
        }
    }
    REQUIRE(observed_fast_batch);
    REQUIRE(observed_replay_batch);
    REQUIRE(observed_penalty_effect);
}

TEST_CASE("exact SIMD hybrid preserves strict equal-score predecessor order",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    params.max_chain_skip = 10000;
    const auto anchors = equal_score_tie_fixture(129u);
    const auto expected = run_scalar(anchors, params);
    const auto actual = run_simd(anchors, params);

    require_same_outputs(actual, expected, "long equal-score ties");
    require_no_forbidden_scalar_work(actual.stats);
    REQUIRE(expected.chains.size() > 0);
    const auto& best_chain = expected.chains.front();
    REQUIRE(best_chain.anchor_count == 129);
    for (std::int32_t offset = 0; offset < best_chain.anchor_count; ++offset) {
        REQUIRE(
            expected.anchors[static_cast<std::size_t>(best_chain.anchor_start + offset)].flags ==
            1u);
    }
    REQUIRE(actual.stats.improvement_replay_batches > 0);
    // Duplicate anchors make every full predecessor batch contain an invalid
    // same-position lane, so the contiguous-store predicate must reject it and
    // preserve the exact masked-scatter fallback.
    REQUIRE(actual.stats.visited_marks > 0);
    REQUIRE(actual.stats.contiguous_predecessor_store_batches == 0);
    REQUIRE(actual.stats.backtrack_primary_contiguous_batches == 0);
}

TEST_CASE("exact SIMD chaining rejects unsupported parameters without fallback",
          "[component:bioseq][layer:unit][feature:chaining][resource:highway]") {
    auto params = exact_params();
    params.max_chain_iter = -1;
    const auto anchors = diagonal_fixture(8);

    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains = ch::make_arena_slab<ch::chain>();
    auto reordered = ch::make_arena_slab<ch::anchor>();
    chains.reserve(anchors.size() + 1);
    reordered.reserve(anchors.size() + 1);
    simd_chaining_execution_stats stats;
    try {
        ch::chain_anchors_simd(fixed_chaining_kernel(), anchors, params, chains, reordered, &stats);
        FAIL("invalid Exact SIMD parameters must throw");
    } catch (const simd_chaining_runtime_error& error) {
        REQUIRE(error.status() == simd_chaining_status::invalid_parameters);
    }
    REQUIRE(stats.fail_closed_rejections == 1);
    REQUIRE(stats.scalar_score_branches == 0);
    REQUIRE(stats.scalar_backtrack_branches == 0);
    REQUIRE(stats.shared_scalar_backtrack_calls == 0);
    REQUIRE(chains.size() == 0);
    REQUIRE(reordered.size() == 0);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
