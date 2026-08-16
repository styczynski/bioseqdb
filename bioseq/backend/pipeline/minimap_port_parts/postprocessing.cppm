/**
 * @file pipeline/minimap_port_parts/postprocessing.cppm
 * @brief Divergence, mapping-quality, and result projection stages.
 */
export module bioseq.pipeline.minimap_port:postprocessing;

import std;
import :results;
import bioseq.common.logging;
import bioseq.highway.dispatch_contract;
import bioseq.chaining.core;
import bioseq.instrumentation;

export namespace bioseq::simd {

// ============================================================================
// Stage 8: Compute mapping quality (mm_set_mapq equivalent)
// ============================================================================

// ============================================================================
// Stage 7b: Estimate divergence from seeds (mm_est_err equivalent)
// ============================================================================

/**
 * @brief Estimate per-base divergence from seed positions.
 *
 * Equivalent of mm_est_err() in esterr.c.
 *
 * Algorithm:
 *   1. Build a sorted array of forward-strand query minimizer positions (mini_pos)
 *   2. For each region, binary-search for the first seed in mini_pos
 *   3. Scan forward through mini_pos, counting how many chain seeds match
 *   4. n_tot = span of mini_pos entries + edge corrections
 *   5. div = 1 - (n_match/n_tot)^(1/avg_k)
 *
 * @param regions           Alignment regions to update
 * @param query_minimizers  All query minimizers (used as mini_pos proxy)
 * @param anchors           Reordered anchor array
 * @param query_len         Query sequence length
 * @param kmer_size         k-mer size
 * @param target_len        Target/reference sequence length
 */
template <class p_minimizer>
void estimate_divergence_from_seeds(std::span<alignment_region> regions,
                                    std::span<const p_minimizer> query_minimizers,
                                    std::span<const anchor> anchors,
                                    sequence_size_t query_len,
                                    int32_t kmer_size,
                                    sequence_size_t target_len) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_est_err");
    auto _scope = bioseq::instrumentation::region(_desc);
    if (query_minimizers.empty()) {
        return;
    }

    // ----------------------------------------------------------------------
    // A sort-free per-position prefix count provides O(1) membership and
    // O(1) range counts:
    //
    //   prefix_count[i] = #(query minimizers with last_pos < i)
    //
    // Then:
    //   * membership at position p:   prefix_count[p+1] > prefix_count[p]
    //   * #mini_pos in [a, b]:         prefix_count[b+1] - prefix_count[a]
    //
    // Build cost is O(N + query_len). The lane-interleaved SIMD collector does
    // not produce sorted positions, so the prefix-count table avoids a sort
    // and answers membership/range queries with two random-access loads.
    // `compute_forward_pos(s)` gives both forward and reverse chains ascending
    // forward positions for s=0..seed_count-1, so one search range is correct
    // for both directions.
    // ----------------------------------------------------------------------
    // For dna4 (the only benchmark alphabet), every minimizer span equals the
    // configured kmer size, so the average span is exactly `kmer_size`.
    const float avg_kmer_span = static_cast<float>(kmer_size);

    // Build the prefix-count array in two linear passes:
    //   1. mini_pos_count[p + 1] += #(minimizers at last_pos == p)
    //   2. cumulative-sum: mini_pos_count[i] += mini_pos_count[i - 1]
    // Final invariant: mini_pos_count[i] = #(minimizers with last_pos < i)
    //
    // Size is (query_len + 2) so `mini_pos_count[query_len + 1]` is a
    // valid index (covers the case where last_pos == query_len, even
    // though collector emission clamps to query_len - 1). Using
    // uint32_t supports up to 4 G minimizers — vastly above realistic
    // cell-genome scale.
    const size_t prefix_size = static_cast<size_t>(query_len) + 2;
    std::vector<uint32_t> mini_pos_count(prefix_size, 0);
    for (const auto& query_min : query_minimizers) {
        const int64_t pos = static_cast<int64_t>(query_min.last_pos());
        if (pos >= 0 && pos <= static_cast<int64_t>(query_len)) {
            ++mini_pos_count[static_cast<size_t>(pos) + 1];
        }
    }
    for (size_t i = 1; i < prefix_size; ++i) {
        mini_pos_count[i] += mini_pos_count[i - 1];
    }

    // Non-capturing functors store their dependencies by value, keeping the hot
    // path inlineable without capture-all lifetime coupling.
    struct is_minimizer {
        const std::uint32_t* mini_pos_count;
        sequence_size_t query_len;
        /// Test whether the prefix table contains one forward position.
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
        /// Count minimizers in one inclusive, clamped position interval.
        [[nodiscard, gnu::always_inline]]
        inline int32_t operator()(std::int64_t a, std::int64_t b) const noexcept {
            if (a < 0)
                a = 0;
            if (static_cast<sequence_size_t>(b) > query_len)
                b = static_cast<std::int64_t>(query_len);
            if (a > b)
                return 0;
            return static_cast<int32_t>(mini_pos_count[static_cast<std::size_t>(b) + 1] -
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

        const int32_t anchor_begin = region.anchor_start;

        // compute_forward_pos walks chain anchors in such order that
        // forward_pos values are MONOTONICALLY ASCENDING in `s`. For forward
        // chains: s -> anchor[s] in chain order (ascending query_position
        // == ascending forward_pos). For reverse chains: s -> anchor[cnt-1-s]
        // in REVERSE chain order; coupled with the y-flip on the
        // reverse-encoded query_position this yields ascending forward_pos.
        // (See esterr.c:50,57-59 — minimap2 walks reverse chains in the
        // same reverse order for the same reason.)
        //
        // Store every dependency by value so the hot-loop callable remains
        // non-capturing and directly inlineable.
        struct compute_forward_position {
            const anchor* anchors_base;
            int32_t anchor_begin;
            int32_t seed_count;
            sequence_size_t query_len;
            bool region_is_reverse;

            /// Project one chain anchor onto its forward query coordinate.
            [[nodiscard, gnu::always_inline]]
            inline std::int64_t operator()(int32_t s) const noexcept {
                const anchor& chain_anchor = region_is_reverse
                                                 ? anchors_base[anchor_begin + seed_count - 1 - s]
                                                 : anchors_base[anchor_begin + s];
                std::int64_t forward_pos = static_cast<std::int64_t>(chain_anchor.query_position);
                if (chain_anchor.is_reverse) {
                    const std::int64_t anchor_span = static_cast<int32_t>(chain_anchor.query_span);
                    forward_pos =
                        static_cast<std::int64_t>(query_len) - 1 - (forward_pos + 1 - anchor_span);
                }
                return forward_pos;
            }
        };
        const compute_forward_position compute_forward_pos{
            anchors.data(), anchor_begin, region.seed_count, query_len, region.is_reverse};

        // First chain anchor (s=0) — minimum forward_pos. esterr.c:50 uses
        // `r->rev ? last : first` which is the same anchor compute_forward_pos(0)
        // returns.
        const std::int64_t first_forward_pos = compute_forward_pos(0);
        if (!is_minimizer(first_forward_pos)) {
            continue;
        }

        // Walk chain anchors with O(1) prefix-count probe. Mirror minimap2's
        // halt-on-miss semantics (esterr.c:57-62 — once a chain anchor is
        // not found in mini_pos, the C `for (...; ++j)` loop advances j to
        // the end without ever advancing k, so subsequent chain anchors
        // are not counted). For our pipeline every chain anchor is by
        // construction a query minimizer (collect_seed_hits looks each one
        // up in the target index), so the loop normally walks all
        // seed_count entries.
        int32_t seed_match_count = 1;
        std::int64_t last_match_pos = first_forward_pos;
        for (int32_t walk_idx = 1; walk_idx < region.seed_count; ++walk_idx) {
            const std::int64_t fp = compute_forward_pos(walk_idx);
            if (!is_minimizer(fp)) {
                break;
            }
            last_match_pos = fp;
            ++seed_match_count;
        }

        // n_tot = span of mini_pos between first and last match (esterr.c:61),
        // counting duplicates (cumulative-sum array preserves them).
        int32_t total_minimizer_span = range_count(first_forward_pos, last_match_pos);

        // Edge corrections (esterr.c:62-63).
        if (region.query_start > avg_kmer_span && region.ref_start > avg_kmer_span) {
            ++total_minimizer_span;
        }
        if (query_len - region.query_start > avg_kmer_span &&
            target_len - region.ref_end > avg_kmer_span) {
            ++total_minimizer_span;
        }

        // Divergence formula (esterr.c:64).
        if (seed_match_count >= total_minimizer_span) {
            region.divergence = 0.0f;
        } else {
            const double match_ratio = static_cast<double>(seed_match_count) / total_minimizer_span;
            region.divergence =
                static_cast<float>(1.0 - std::pow(match_ratio, 1.0 / avg_kmer_span));
        }
    }
}

// ============================================================================
// Stage 8: Compute mapping quality (mm_set_mapq equivalent)

/**
 * @brief Compute mapping quality for all regions.
 *
 * Equivalent to mm_set_mapq2() in hit.c:423-476.
 *
 * MAPQ formula (primary with DP scores):
 *   mapq = identity * pen_cm * 40 * (1 - ratio^2) * ln(dp_max_score / match_sc)
 *   mapq -= 4.343 * ln(secondary_count + 1)
 *   mapq = clamp(mapq, 0, 60)
 *
 * Where ratio = secondary_dp_max * secondary_chain_score / (dp_max_score * original_score)
 */
void compute_mapping_quality(std::span<alignment_region> regions,
                             const minimap2_parameters& params,
                             int32_t rep_len) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_set_mapq");
    auto _scope = bioseq::instrumentation::region(_desc);
    constexpr float quality_coefficient = 40.0f;
    const int32_t match_sc = params.match_score;
    const int32_t min_chain_sc = params.min_chain_score;

    // Compute uniqueness ratio
    int64_t primary_score_sum = 0;
    for (const auto& region : regions) {
        if (region.parent == region.region_id) {
            primary_score_sum += region.score;
        }
    }
    // hit.c:436: uniq_ratio = (float)sum_sc / (sum_sc + rep_len)
    const float uniqueness_ratio =
        static_cast<float>(primary_score_sum) / static_cast<float>(primary_score_sum + rep_len);

    for (auto& region : regions) {
        if (region.parent != region.region_id) {
            region.mapq = 0; // secondaries get 0
            continue;
        }

        // Primary mapping
        const float score_factor = (region.score > 100) ? 1.0f : 0.01f * region.score;
        const float score_penalty = score_factor * uniqueness_ratio;
        const float raw_seed_penalty = (region.seed_count > 10) ? 1.0f : 0.1f * region.seed_count;
        const float seed_count_penalty = std::min(score_penalty, raw_seed_penalty);

        const int32_t effective_secondary_score = std::max(region.secondary_score, min_chain_sc);

        int32_t quality_value;
        // hit.c:447-467: three branches based on DP availability
        if (region.has_alignment && region.secondary_dp_max > 0 && region.dp_max_score > 0) {
            // Formula A: with DP scores (hit.c:448-457)
            const float identity =
                (region.block_length > 0)
                    ? static_cast<float>(region.match_length) / region.block_length
                    : 0.f;
            const float score_ratio =
                static_cast<float>(region.secondary_dp_max) * effective_secondary_score /
                (static_cast<float>(region.dp_max_score) * region.original_score);
            quality_value =
                static_cast<int32_t>(identity * seed_count_penalty * quality_coefficient *
                                     (1.0f - score_ratio * score_ratio) *
                                     std::log(static_cast<float>(region.dp_max_score) / match_sc));

            // BWA-MEM-like alternative (hit.c:455)
            const int32_t quality_alt = static_cast<int32_t>(
                6.02f * identity * identity * (region.dp_max_score - region.secondary_dp_max) /
                    match_sc +
                0.499f);
            quality_value = std::min(quality_value, quality_alt);
        } else {
            // No secondary_dp_max available: use chain score formula (hit.c:460-467)
            const float score_ratio =
                static_cast<float>(effective_secondary_score) / region.original_score;
            if (region.has_alignment) {
                const float identity =
                    (region.block_length > 0)
                        ? static_cast<float>(region.match_length) / region.block_length
                        : 0.f;
                quality_value = static_cast<int32_t>(
                    identity * seed_count_penalty * quality_coefficient * (1.0f - score_ratio) *
                    std::log(static_cast<float>(region.dp_max_score) / match_sc));
            } else {
                // No DP at all — use chain score (hit.c:466)
                quality_value = static_cast<int32_t>(seed_count_penalty * quality_coefficient *
                                                     (1.0f - score_ratio) *
                                                     std::log(static_cast<float>(region.score)));
            }
        }

        // Subtract secondary_count penalty (hit.c:469)
        quality_value -= static_cast<int32_t>(
            4.343f * std::log(static_cast<float>(region.secondary_count + 1)) + 0.499f);
        quality_value = std::max(quality_value, 0);
        region.mapq = std::min(quality_value, 60);

        // Ensure non-zero if clearly best (hit.c:472)
        if (region.has_alignment && region.dp_max_score > region.secondary_dp_max &&
            region.mapq == 0) {
            region.mapq = 1;
        }
    }

    bioseq_log<logging_level::debug>("[minimap_port] Computed MAPQ for {} regions", regions.size());
}

// ============================================================================
// Stage 9: Convert to match_result (PAF output)
// ============================================================================

/**
 * @brief Convert internal alignment_region to PAF-like match_result.
 *
 * Fields map to PAF columns as documented in mm_write_paf4() (format.c:427-460).
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
