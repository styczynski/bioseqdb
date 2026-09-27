/**
 * @file pipeline/minimap_port_parts/postprocessing.cppm
 * @brief Estimates divergence and mapping quality, then prepares match results.
 */
module;

#include <new>

export module bioseq.pipeline.minimap_port:postprocessing;

import std;
import :results;
import bioseq.common.logging;
import bioseq.highway.dispatch_contract;
import bioseq.chaining.core;
import bioseq.instrumentation;

export namespace bioseq::simd {

// Alignment postprocessing.

// Estimate divergence from chain seeds.

/**
 * @brief Estimate sequence divergence from the fraction of query seeds in a chain.
 *
 * Count minimizers in the matched span, with allowances for unaligned edges.
 * Fewer matching seeds mean higher estimated divergence. Only positions are
 * compared.
 *
 * @param regions Regions whose divergence values are updated.
 * @param query_minimizers All query minimizers, in any order.
 * @param anchors Chain anchors in each region's recorded range.
 * @param query_len Query length in bases.
 * @param kmer_size Seed length used by the estimate.
 * @param target_len Target length in bases.
 */
template <class p_minimizer>
void estimate_divergence_from_seeds(std::span<alignment_region> regions,
                                    std::span<const p_minimizer> query_minimizers,
                                    std::span<const anchor> anchors,
                                    sequence_size_t query_len,
                                    std::int32_t kmer_size,
                                    sequence_size_t target_len) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_est_err");
    auto _scope = bioseq::instrumentation::region(_desc);
    if (query_minimizers.empty()) {
        return;
    }

    // Use the configured k-mer length as the seed span in the estimate.
    const float avg_kmer_span = static_cast<float>(kmer_size);

    // Prefix counts let us test seed positions and count minimizers in a range
    // without sorting SIMD output. Duplicates still contribute to the count.
    // The extra entry makes a position equal to query_len safe to query.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (query_len >
        static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)() - 2u)) {
        throw std::length_error{"divergence prefix count is not representable"};
    }
    const std::size_t prefix_size = static_cast<std::size_t>(query_len) + 2u;
    alignment_result_vector<std::uint32_t> mini_pos_count;
    mini_pos_count.set_logical_bounds(prefix_size, prefix_size);
    mini_pos_count.resize(prefix_size);
#else
    const std::size_t prefix_size = static_cast<std::size_t>(query_len) + 2;
    std::vector<std::uint32_t> mini_pos_count(prefix_size, 0);
#endif
    for (const auto& query_min : query_minimizers) {
        const std::int64_t pos = static_cast<std::int64_t>(query_min.last_pos());
        if (pos >= 0 && pos <= static_cast<std::int64_t>(query_len)) {
            ++mini_pos_count[static_cast<std::size_t>(pos) + 1];
        }
    }
    for (std::size_t i = 1; i < prefix_size; ++i) {
        mini_pos_count[i] += mini_pos_count[i - 1];
    }

    // These helpers borrow the prefix table for the duration of this call.
    struct is_minimizer {
        const std::uint32_t* mini_pos_count;
        sequence_size_t query_len;
        /// Check whether a minimizer ends at this forward query position.
        [[nodiscard, gnu::always_inline]]
        inline bool operator()(std::int64_t pos) const noexcept {
            if (pos < 0 || static_cast<sequence_size_t>(pos) > query_len)
                return false;
            const std::size_t p = static_cast<std::size_t>(pos);
            return mini_pos_count[p + 1] > mini_pos_count[p];
        }
    };
    struct range_count {
        const std::uint32_t* mini_pos_count;
        sequence_size_t query_len;
        /// Count minimizers between the supplied positions, including both ends.
        [[nodiscard, gnu::always_inline]]
        inline std::int32_t operator()(std::int64_t a, std::int64_t b) const noexcept {
            if (a < 0)
                a = 0;
            if (static_cast<sequence_size_t>(b) > query_len)
                b = static_cast<std::int64_t>(query_len);
            if (a > b)
                return 0;
            return static_cast<std::int32_t>(
                mini_pos_count[static_cast<std::size_t>(b) + 1] -
                mini_pos_count[static_cast<std::size_t>(a)]);
        }
    };
    const is_minimizer is_minimizer{mini_pos_count.data(), query_len};
    const range_count range_count{mini_pos_count.data(), query_len};

    for (auto& region : regions) {
        region.divergence = -1.0f;
        if (region.seed_count <= 0) {
            continue;
        }

        const std::int32_t anchor_begin = region.anchor_start;

        // Reverse chains store query coordinates in the opposite direction.
        // Walk their anchors backwards and convert the coordinates so both
        // strands visit forward query positions in increasing order.
        struct compute_forward_position {
            const anchor* anchors_base;
            std::int32_t anchor_begin;
            std::int32_t seed_count;
            sequence_size_t query_len;
            bool region_is_reverse;

            /// Return an anchor's end position on the forward query strand.
            [[nodiscard, gnu::always_inline]]
            inline std::int64_t operator()(std::int32_t s) const noexcept {
                const anchor& chain_anchor = region_is_reverse
                                                 ? anchors_base[anchor_begin + seed_count - 1 - s]
                                                 : anchors_base[anchor_begin + s];
                std::int64_t forward_pos = static_cast<std::int64_t>(chain_anchor.query_position);
                if (chain_anchor.is_reverse) {
                    const std::int64_t anchor_span =
                        static_cast<std::int32_t>(chain_anchor.query_span);
                    forward_pos =
                        static_cast<std::int64_t>(query_len) - 1 - (forward_pos + 1 - anchor_span);
                }
                return forward_pos;
            }
        };
        const compute_forward_position compute_forward_pos{
            anchors.data(), anchor_begin, region.seed_count, query_len, region.is_reverse};

        // Start at the chain's leftmost seed on the forward query strand.
        const std::int64_t first_forward_pos = compute_forward_pos(0);
        if (!is_minimizer(first_forward_pos)) {
            continue;
        }

        // Stop at the first chain position missing from the query minimizers.
        // This keeps the same stop rule as minimap2's seed-based estimate.
        std::int32_t seed_match_count = 1;
        std::int64_t last_match_pos = first_forward_pos;
        for (std::int32_t walk_idx = 1; walk_idx < region.seed_count; ++walk_idx) {
            const std::int64_t fp = compute_forward_pos(walk_idx);
            if (!is_minimizer(fp)) {
                break;
            }
            last_match_pos = fp;
            ++seed_match_count;
        }

        // Count all query minimizers in the matched span, including duplicates.
        std::int32_t total_minimizer_span = range_count(first_forward_pos, last_match_pos);

        // Add edge allowances when sequence extends beyond the seed span.
        if (region.query_start > avg_kmer_span && region.ref_start > avg_kmer_span) {
            ++total_minimizer_span;
        }
        if (query_len - region.query_start > avg_kmer_span &&
            target_len - region.ref_end > avg_kmer_span) {
            ++total_minimizer_span;
        }

        // Convert the matched-seed fraction into a per-base divergence estimate.
        if (seed_match_count >= total_minimizer_span) {
            region.divergence = 0.0f;
        } else {
            const double match_ratio = static_cast<double>(seed_match_count) / total_minimizer_span;
            region.divergence =
                static_cast<float>(1.0 - std::pow(match_ratio, 1.0 / avg_kmer_span));
        }
    }
}

// Compute mapping quality.

/**
 * @brief Estimate confidence in each region's placement.
 *
 * Primary regions get a score from 0 to 60. Close competing hits, few seeds,
 * and repetitive sequence reduce that score. Alignment scores and identity
 * refine the estimate when available, otherwise it uses chain scores.
 * Secondary regions get zero.
 *
 * rep_len is the repetitive query length used to reduce confidence.
 */
void compute_mapping_quality(std::span<alignment_region> regions,
                             const minimap2_parameters& params,
                             std::int32_t rep_len) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_set_mapq");
    auto _scope = bioseq::instrumentation::region(_desc);
    constexpr float quality_coefficient = 40.0f;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto tail_storage_plan = derive_alignment_tail_storage_plan(regions.size());
    if (!tail_storage_plan || tail_storage_plan->mapq_region_count != regions.size()) {
        throw std::length_error{"MAPQ region count is not representable"};
    }
#endif
    const std::int32_t match_sc = params.match_score;
    const std::int32_t min_chain_sc = params.min_chain_score;

    // Sum primary chain scores to estimate support outside repetitive sequence.
    std::int64_t primary_score_sum = 0;
    for (const auto& region : regions) {
        if (region.parent == region.region_id) {
            primary_score_sum += region.score;
        }
    }
    // Repetitive query bases reduce the weight of that support.
    const float uniqueness_ratio =
        static_cast<float>(primary_score_sum) / static_cast<float>(primary_score_sum + rep_len);

    for (auto& region : regions) {
        if (region.parent != region.region_id) {
            region.mapq = 0; // Secondary hit.
            continue;
        }

        // Reduce confidence when the primary has few seeds or weak chain support.
        const float score_factor = (region.score > 100) ? 1.0f : 0.01f * region.score;
        const float score_penalty = score_factor * uniqueness_ratio;
        const float raw_seed_penalty = (region.seed_count > 10) ? 1.0f : 0.1f * region.seed_count;
        const float seed_count_penalty = std::min(score_penalty, raw_seed_penalty);

        const std::int32_t effective_secondary_score =
            std::max(region.secondary_score, min_chain_sc);

        std::int32_t quality_value;
        // Use alignment evidence when available.
        if (region.has_alignment && region.secondary_dp_max > 0 && region.dp_max_score > 0) {
            // Compare the primary and competing alignment scores.
            const float identity =
                (region.block_length > 0)
                    ? static_cast<float>(region.match_length) / region.block_length
                    : 0.f;
            const float score_ratio =
                static_cast<float>(region.secondary_dp_max) * effective_secondary_score /
                (static_cast<float>(region.dp_max_score) * region.original_score);
            quality_value =
                static_cast<std::int32_t>(
                    identity * seed_count_penalty * quality_coefficient *
                    (1.0f - score_ratio * score_ratio) *
                    std::log(static_cast<float>(region.dp_max_score) / match_sc));

            // Also cap confidence by the gap between the two alignment scores.
            const std::int32_t quality_alt = static_cast<std::int32_t>(
                6.02f * identity * identity * (region.dp_max_score - region.secondary_dp_max) /
                    match_sc +
                0.499f);
            quality_value = std::min(quality_value, quality_alt);
        } else {
            // Use competing chain scores when a secondary alignment score is missing.
            const float score_ratio =
                static_cast<float>(effective_secondary_score) / region.original_score;
            if (region.has_alignment) {
                const float identity =
                    (region.block_length > 0)
                        ? static_cast<float>(region.match_length) / region.block_length
                        : 0.f;
                quality_value = static_cast<std::int32_t>(
                    identity * seed_count_penalty * quality_coefficient * (1.0f - score_ratio) *
                    std::log(static_cast<float>(region.dp_max_score) / match_sc));
            } else {
                // Estimate confidence from the chain when alignment was skipped.
                quality_value = static_cast<std::int32_t>(
                    seed_count_penalty * quality_coefficient * (1.0f - score_ratio) *
                    std::log(static_cast<float>(region.score)));
            }
        }

        // More competing hits reduce confidence.
        quality_value -= static_cast<std::int32_t>(
            4.343f * std::log(static_cast<float>(region.secondary_count + 1)) + 0.499f);
        quality_value = std::max(quality_value, 0);
        region.mapq = std::min(quality_value, 60);

        // Keep a minimum score of one for an alignment that beats its competitor.
        if (region.has_alignment && region.dp_max_score > region.secondary_dp_max &&
            region.mapq == 0) {
            region.mapq = 1;
        }
    }

    bioseq_log<logging_level::debug>("[minimap_port] Computed MAPQ for {} regions", regions.size());
}

// Prepare the public match result.

/**
 * @brief Copy region coordinates and scores into a match result.
 *
 * The result carries PAF-style fields together with BioseqDB's chain and
 * alignment details. Ownership of the CIGAR moves to the result.
 */
[[nodiscard]] match_result region_to_match_result(alignment_region&& region,
                                                  sequence_size_t query_len,
                                                  sequence_size_t target_len) {
    return match_result{
        .query_len = query_len,
        .query_start = region.query_start,
        .query_end = region.query_end,
        .is_reverse = region.is_reverse,
        .target_len = target_len,
        .target_start = region.ref_start,
        .target_end = region.ref_end,
        .num_matches = region.match_length,
        .block_length = region.block_length,
        .mapq = region.mapq,
        .chain_score = region.score,
        .dp_score = region.dp_score,
        .dp_max_score = region.dp_max_score,
        .seed_count = region.seed_count,
        .secondary_chain_score = region.secondary_score,
        .divergence = region.divergence,
        .is_primary = (region.parent == region.region_id),
        .has_cigar = region.has_alignment,
        .cigar = std::move(region.cigar),
        .edit_distance = region.edit_distance,
    };
}

} // namespace bioseq::simd
