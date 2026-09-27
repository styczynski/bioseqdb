/**
 * @file chaining_simd_kernel.cppm
 * @brief Select a SIMD chaining policy, score anchors, and recover chains.
 *
 * Highway generates an entry point for each supported CPU target from this
 * module. The imported modules prepare anchors, score possible predecessors,
 * and follow the chosen predecessors to produce chains.
 */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

export module bioseq.highway.sources.chaining;

import std;
import bioseq.common.context;
import bioseq.common.logging;
import bioseq.instrumentation;
import bioseq.chaining.simd;
import bioseq.chaining.core;
import bioseq.highway.sources.chaining_candidate_scoring;
import bioseq.highway.sources.chaining_forward;
import bioseq.highway.sources.chaining_backtracking;

namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using bioseq::chaining::anchor;
using bioseq::chaining::arena_slab;
using bioseq::chaining::chain;
using bioseq::chaining::chaining_parameters;
using bioseq::chaining::simd_chaining_execution_stats;
using bioseq::chaining::simd_chaining_policy_stats;
using bioseq::chaining::simd_chaining_policy;
using bioseq::chaining::simd_chaining_runtime_error;
using bioseq::chaining::simd_chaining_status;

namespace chaining_simd_detail {

/// @brief Score anchors with the requested SIMD policy and recover their chains.
/// @tparam p_collect_stats Collect counters inside the scoring loops.
/// @tparam p_descriptor_type Highway descriptor for the selected CPU target.
/// @param d Descriptor that determines the SIMD lane type and count.
/// @param anchors Anchors in canonical_anchor_less order.
/// @param params Scoring rules, distance limits, and SIMD policy.
/// @param chains_out Receives chain descriptors in arena storage.
/// @param reordered_anchors_out Receives anchors grouped in chain order.
/// @param stats Optional detailed execution counters.
/// @param route_summary Optional counters collected without tracing each scoring step.
template<bool p_collect_stats, class p_descriptor_type>
void run_simd_chaining(
    p_descriptor_type d,
    std::span<const anchor> anchors,
    const chaining_parameters& params,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out,
    simd_chaining_execution_stats* stats,
    simd_chaining_route_stats* route_summary)
{
    const auto storage_plan = bioseq::chaining::derive_simd_chaining_storage_plan(
        anchors.size(), params);
    if (!storage_plan || !storage_plan->logical_scratch_bytes(hn::Lanes(d))) {
        reject(stats, simd_chaining_status::workspace_overflow,
               "SIMD chaining storage plan exceeds its addressable domain");
    }
    const std::size_t count = storage_plan->output.anchor_count;
    auto soa = create_soa_anchors(d, anchors);
    auto best_scores =
        context::current().alloc_chain_array<std::int32_t>(count);
    auto predecessors =
        context::current().alloc_chain_array<std::int32_t>(count);
    auto peak_scores =
        context::current().alloc_chain_array<std::int32_t>(count);
    std::memset(
        predecessors.data(), 0xFF,
        count * sizeof(std::int32_t));

    switch (params.simd_policy) {
        case simd_chaining_policy::exact_visited:
            simd_dp_forward_pass<
                simd_chaining_policy::exact_visited, p_collect_stats>(
                    d,
                    anchors,
                    soa,
                    best_scores,
                    predecessors,
                    peak_scores,
                    params,
                    stats,
                    route_summary);
            break;
        case simd_chaining_policy::bounded_batch_approximate:
            simd_dp_forward_pass<
                simd_chaining_policy::bounded_batch_approximate, p_collect_stats>(
                    d,
                    anchors,
                    soa,
                    best_scores,
                    predecessors,
                    peak_scores,
                    params,
                    stats,
                    route_summary);
            break;
        default:
            reject(
                stats,
                simd_chaining_status::invalid_policy,
                "unknown SIMD chaining policy reached forward dispatch");
    }
    {
        // Time the SIMD backtracking call itself so this counter reports only
        // the work that follows predecessors and writes chains.
        static const auto* backtrack_region_desc =
            bioseq::instrumentation::register_static_region("backtrack_chains");
        auto backtrack_region_scope =
            bioseq::instrumentation::region(backtrack_region_desc);
        backtrack_chains_simd<p_collect_stats>(
            d,
            anchors,
            best_scores,
            predecessors,
            params.min_chain_score,
            params.min_seed_count,
            chains_out,
            reordered_anchors_out,
            stats,
            route_summary);
    }
}

}  // namespace chaining_simd_detail

/**
 * @brief Chain sorted anchors using SIMD on the selected CPU target.
 *
 * Both scoring and backtracking use SIMD. Invalid settings, unsupported CPU
 * targets, and inputs too large for the scratch arrays raise an error.
 * Counters are optional.
 *
 * @param anchors Anchors in canonical_anchor_less order.
 * @param params Scoring rules, distance limits, and SIMD policy.
 * @param chains_out Receives chain descriptors in arena storage.
 * @param reordered_anchors_out Receives anchors grouped in chain order.
 * @param stats Optional counters, reset at the start of this call.
 * @param aggregate_stats Collect a shorter set of counters when stats is supplied.
 * @throws simd_chaining_runtime_error If the settings, CPU target, or scratch size are unsupported.
 */
void chain_anchors_simd(
    std::span<const anchor> anchors,
    const chaining_parameters& params,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out,
    simd_chaining_execution_stats* stats = nullptr,
    bool aggregate_stats = false)
{
    if (stats != nullptr) {
        *stats = {};
        stats->anchors_consumed = anchors.size();
    }

    hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    switch (params.simd_policy) {
        case simd_chaining_policy::exact_visited:
        case simd_chaining_policy::bounded_batch_approximate:
            break;
        default:
            chaining_simd_detail::reject(
                stats,
                simd_chaining_status::invalid_policy,
                "unknown SIMD chaining policy");
    }
    if (lanes < 2) {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::unsupported_simd_target,
            "the selected Highway target has fewer than two int32 lanes");
    }
    if (params.max_gap < 0
        || params.bandwidth < 0
        || params.max_chain_skip < 0
        || params.max_chain_iter < 0
        || params.min_seed_count < 0
        || !std::isfinite(params.gap_penalty)
        || !std::isfinite(params.skip_penalty)
        || params.gap_penalty < 0.0F
        || params.skip_penalty < 0.0F)
    {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::invalid_parameters,
            "distances, iteration limits, seed count, and penalties must "
            "be finite and non-negative");
    }
    if (params.simd_policy
            == simd_chaining_policy::bounded_batch_approximate
        && (params.max_chain_skip <= 0
            || params.max_chain_iter <= 0))
    {
        if (stats != nullptr) {
            ++stats->policy.policy_rejections;
        }
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::invalid_parameters,
            "bounded SIMD chaining requires positive max_chain_skip and "
            "max_chain_iter");
    }
    if (anchors.size()
        > static_cast<std::size_t>(
            std::numeric_limits<std::int32_t>::max()))
    {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::anchor_count_overflow,
            "anchor indices do not fit the exact int32 SIMD ABI");
    }
    if (!bioseq::chaining::simd_chaining_path_elements(anchors.size(), lanes))
    {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::workspace_overflow,
            "lane-batched backtracking workspace exceeds int32 gather ABI");
    }
    if (stats != nullptr) {
        switch (params.simd_policy) {
            case simd_chaining_policy::exact_visited:
                ++stats->policy.exact_visited_calls;
                break;
            case simd_chaining_policy::bounded_batch_approximate:
                ++stats->policy.bounded_batch_approximate_calls;
                break;
            default:
                // The earlier policy check rejects every value outside these two cases.
                chaining_simd_detail::reject(
                    stats,
                    simd_chaining_status::invalid_policy,
                    "unknown SIMD chaining policy after validation");
        }
    }
    if (anchors.empty()) {
        return;
    }

    static const auto* region_desc =
        bioseq::instrumentation::register_static_region("mg_lchain_simd");
    auto region_scope = bioseq::instrumentation::region(region_desc);
    auto chain_scope =
        context::current().push<context::arena::chain>();

    if (stats != nullptr && aggregate_stats) {
        chaining_simd_detail::simd_chaining_route_stats route_summary{};
        chaining_simd_detail::run_simd_chaining<false>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            nullptr,
            &route_summary);
        stats->score_vector_batches =
            route_summary.score_vector_batches;
        stats->score_vector_lanes = route_summary.score_vector_lanes;
        stats->score_masked_lanes = route_summary.score_masked_lanes;
        stats->no_improvement_fast_batches =
            route_summary.no_improvement_fast_batches;
        stats->improvement_replay_batches =
            route_summary.improvement_replay_batches;
        stats->improvement_replay_lanes =
            route_summary.improvement_replay_lanes;
        stats->contiguous_predecessor_store_batches =
            route_summary.contiguous_predecessor_store_batches;
        stats->scalar_orchestration_steps =
            route_summary.scalar_orchestration_steps;
        stats->global_best_vector_batches =
            route_summary.global_best_vector_work ? 1u : 0u;
        stats->global_best_vector_lanes =
            route_summary.global_best_vector_work ? 1u : 0u;
        stats->backtrack_rank_batches = route_summary.backtrack_rank_work ? 1u : 0u;
        stats->backtrack_path_vector_batches =
            route_summary.backtrack_path_vector_work ? 1u : 0u;
        stats->backtrack_commit_vector_batches =
            route_summary.backtrack_commit_vector_work ? 1u : 0u;
        stats->backtrack_vector_lanes =
            route_summary.backtrack_path_vector_work ? 1u : 0u;
        stats->backtrack_primary_contiguous_batches =
            route_summary.backtrack_primary_contiguous_batches;
        stats->backtrack_all_starts_visited_fast_batches =
            route_summary.backtrack_all_starts_visited_fast_batches;
        if (params.simd_policy
            == simd_chaining_policy::bounded_batch_approximate)
        {
            simd_chaining_policy_stats bounded_stats{
                .bounded_score_batches =
                    route_summary.bounded_score_batches,
                .bounded_improving_batches =
                    route_summary.bounded_improving_batches,
                .bounded_nonimproving_batches =
                    route_summary.bounded_nonimproving_batches,
                .bounded_cutoff_stops =
                    route_summary.bounded_cutoff_stops,
                .bounded_candidates_skipped =
                    route_summary.bounded_candidates_skipped,
                .global_best_rescue_probes =
                    route_summary.global_best_rescue_probes,
                .global_best_rescue_wins =
                    route_summary.global_best_rescue_wins,
            };
            stats->policy.merge(bounded_stats);
        }
    } else if (stats != nullptr) {
        chaining_simd_detail::run_simd_chaining<true>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            stats,
            nullptr);
    } else {
        chaining_simd_detail::run_simd_chaining<false>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            nullptr,
            nullptr);
    }
    bioseq_log<logging_level::debug>(
        "[chaining_simd] {} SIMD chaining complete for {} anchors",
        bioseq::chaining::simd_chaining_policy_name(
            params.simd_policy),
        anchors.size());
}

}  // namespace target
}  // namespace bioseq::simd
