/**
 * @file pipeline/minimap_port_parts/seeding_chaining.cppm
 * @brief Seed lookup, chaining-region construction, and region selection.
 */
export module bioseq.pipeline.minimap_port:seeding_chaining;

import std;
import :results;
import bioseq.common.logging;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.instrumentation;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;

export namespace bioseq::simd {

// ============================================================================
// Internal data structures
// ============================================================================
// Anchor and chain ownership resides in bioseq.chaining.core.

// Alignment-region and logarithm helpers are owned by their alignment and
// chaining modules.

// ============================================================================
// Stage 1: Build target index from target minimizers
// ============================================================================

// Target index types and lookup concepts are owned by the indexing modules.

// ============================================================================
// Stage 2: Collect seed hits — look up query minimizers in target index
// ============================================================================

/**
 * Append one borrowed packed target-position run for a single query
 * minimizer. Position width is resolved once by the caller, so a hash-major
 * uint32 posting remains packed through the seed loop and is widened only in
 * the register used to construct its anchor.
 */
template <std::unsigned_integral p_stored_position>
inline void
append_seed_hits_from_position_span(std::span<const p_stored_position> target_hits,
                                    std::uint32_t query_strand_bit,
                                    sequence_size_t forward_query_pos,
                                    sequence_size_t reverse_query_pos,
                                    std::uint32_t query_span_value,
                                    ::bioseq::chaining::arena_slab<anchor>& anchors_out) {
    for (const p_stored_position stored_target_hit : target_hits) {
        const std::uint64_t target_hit = static_cast<std::uint64_t>(stored_target_hit);
        const sequence_size_t ref_position = static_cast<sequence_size_t>(target_hit >> 1u);
        const std::uint32_t target_strand_bit = static_cast<std::uint32_t>(target_hit & 1u);
        const std::uint32_t is_reverse_bit = target_strand_bit ^ query_strand_bit;
        const sequence_size_t selected_query_pos =
            is_reverse_bit ? reverse_query_pos : forward_query_pos;

        anchor anchor;
        anchor.ref_position = ref_position;
        anchor.is_reverse = is_reverse_bit;
        anchor.query_position = selected_query_pos;
        anchor.query_span = static_cast<std::uint8_t>(query_span_value);
        anchor.flags = 0;
        anchor.segment_id = 0;
        anchors_out.push_back(anchor);
    }
}

/// Sort seed anchors into canonical reference/query order.
inline void sort_seed_hits(::bioseq::chaining::arena_slab<anchor>& anchors_out) {
    static const auto* _sort_desc =
        bioseq::instrumentation::register_static_region("collect_seed_hits_sort");
    auto _sort_scope = bioseq::instrumentation::region(_sort_desc);
    if (anchors_out.size() < 2u)
        return;
    (void)::bioseq::chaining::sort_canonical_anchors(
        std::span<anchor>{anchors_out.data(), anchors_out.size()});
}

/**
 * @brief Collect seed anchors by looking up query minimizers in the target index.
 *
 * Equivalent to collect_seed_hits() in map.c:518-554.
 *
 * For each query minimizer, look up its hash in the target index. For each
 * hit, create an anchor encoding both reference and query positions.
 *
 * anchor encoding (same as minimap2):
 *   ref_bits: is_reverse<<63 | reference_id<<32 | ref_position
 *   query_bits: query_span<<32 | query_position
 *
 * Where for forward strand: ref_position = target_pos, query_position = query_pos
 * For reverse strand: set is_reverse bit, adjust query_position for reverse complement.
 *
 * @param query_minimizers Precomputed query minimizers
 * @param target_index     Target index to search
 * @param query_len        Length of query sequence
 * @param max_occ          Max occurrences to keep (filter high-frequency seeds)
 * @param kmer_size        K-mer length used for the encoded anchor span.
 * @param anchors_out      Output slab — sorted anchors are appended here.
 *                          Must be a live arena_slab rooted in the caller's
 *                          per-query `Arena::chain` scope. The kernel writes
 *                          directly into it via `push_back` and sorts in
 *                          place before returning.
 */
template <class p_minimizer, minimizer_lookup_index p_index>
void collect_seed_hits(std::span<const p_minimizer> query_minimizers,
                       const p_index& target_index,
                       sequence_size_t query_len,
                       int32_t max_occ,
                       int32_t kmer_size,
                       ::bioseq::chaining::arena_slab<anchor>& anchors_out) {
    static const auto* _seed_hits_desc =
        bioseq::instrumentation::register_static_region("collect_seed_hits");
    auto _seed_hits_scope = bioseq::instrumentation::region(_seed_hits_desc);
    // The caller is expected to pre-reserve the slab using the
    // `estimated_anchor_count` helper from `bioseq::simd::pipe::detail`
    // — that estimate is tighter than `query_minimizers.size() * 2` for very-short
    // sequences (clamped by `shared_ceiling`) and identical for the common case.
    // We still issue a defensive `reserve` here so the kernel works when invoked
    // directly with a fresh slab; the call is a no-op when `cap_` already covers
    // the requested capacity (see `arena_slab::reserve` short-circuit).
    anchors_out.reserve(query_minimizers.size() * 2);

    // Target-index lookup is the hot per-minimizer operation. We wrap the
    // whole loop (not each lookup) to avoid profiler overhead dominating
    // the measurement; the loop body also classifies/sorts anchors.
    {
        static const auto* _idx_lookup_desc =
            bioseq::instrumentation::register_static_region("target_index_lookup");
        auto _idx_lookup_scope = bioseq::instrumentation::region(_idx_lookup_desc);
        // On the dna4 benchmark path every minimizer span equals the configured
        // kmer size, so hoist that constant outside the lookup loop.
        const uint32_t query_span_value = static_cast<uint32_t>(kmer_size);
        // Precompute the constant added to the negated forward_query_pos in the
        // reverse-strand branch:
        //   reverse_query_pos = query_len - (fwd + 1 - span) - 1
        //                     = (query_len - 2 + span) - fwd
        // Hoisting this addition out of the inner loop lets the compiler keep
        // (sub fwd) as the only per-hit op for the reverse branch.
        const sequence_size_t reverse_pos_base = query_len - 2 + query_span_value;
        if (max_occ < 0) [[unlikely]] {
            throw std::invalid_argument("collect_seed_hits requires a nonnegative max_occ");
        }
        for (const auto& query_minimizer : query_minimizers) {
            const minimizer_lookup_spans target_hits =
                lookup_minimizer_spans(target_index, query_minimizer.kmer());
            const std::uint64_t global_occurrence_count = target_hits.global_occurrence_count();
            if (global_occurrence_count == 0 ||
                global_occurrence_count > static_cast<std::uint64_t>(max_occ)) {
                continue;
            }
            if (!target_hits.positions_complete() || !target_hits.well_formed()) {
                throw bioseq::indexing::persisted_minimizer_lookup_error(
                    "persisted minimizer positions unavailable or inconsistent "
                    "after global occurrence pruning");
            }

            // `pos_strand_encoded()` returns `(last_pos << 1) | strand` — the
            // exact form `lookup()` packed into the positions array, so the
            // per-hit `target_hit` lives in the same encoded space.
            // — keep the FULL 64-bit encoded value. `pos_bits` is
            // `ceil(log2(seq_len))` (up to 32 for a multi-Gbp sequence), so
            // `(last_pos << 1) | strand` spans up to 33 bits; the old
            // `static_cast<uint32_t>(...)` truncated any position >= 2^31. The
            // query is small here, but mirroring the target-side fix below keeps
            // both sides 64-bit-correct.
            const uint64_t query_pos_encoded = query_minimizer.pos_strand_encoded();
            // Hoist the strand bit out of the inner per-hit loop — constant
            // across `target_hits`.
            const uint32_t query_strand_bit = static_cast<uint32_t>(query_pos_encoded & 1ULL);
            const sequence_size_t forward_query_pos =
                static_cast<sequence_size_t>(query_pos_encoded >> 1);
            // Reverse-strand position formula reduces to a single subtraction
            // once `reverse_pos_base` is hoisted; pre-compute here so the
            // branchless `csel` below picks between two register-resident
            // 64-bit positions. (forward_query_pos is a k-mer start <= query_len-k,
            // so reverse_pos_base - forward_query_pos never underflows.)
            const sequence_size_t reverse_query_pos = reverse_pos_base - forward_query_pos;

            const auto contiguous32 = target_hits.contiguous_positions32();
            const auto contiguous64 = target_hits.contiguous_positions();
            if (!contiguous32.empty()) {
                append_seed_hits_from_position_span(contiguous32,
                                                    query_strand_bit,
                                                    forward_query_pos,
                                                    reverse_query_pos,
                                                    query_span_value,
                                                    anchors_out);
            } else if (!contiguous64.empty()) {
                append_seed_hits_from_position_span(contiguous64,
                                                    query_strand_bit,
                                                    forward_query_pos,
                                                    reverse_query_pos,
                                                    query_span_value,
                                                    anchors_out);
            } else {
                for (const minimizer_position_span& target_span :
                     target_hits.segmented_positions()) {
                    append_seed_hits_from_position_span(target_span.positions,
                                                        query_strand_bit,
                                                        forward_query_pos,
                                                        reverse_query_pos,
                                                        query_span_value,
                                                        anchors_out);
                }
            }
        }
    } // end target_index_lookup scope

    // Establish the canonical `(ref_bits, query_bits, full query_position)`
    // order. The primary reference-key pass remains the exact port of
    // minimap2's `radix_sort_128x(a, a + n_a)` from misc.c; equal-reference
    // runs are then comparison-sorted by `sort_seed_hits`. The
    // 8-bit-per-pass MSD radix sort is faster than libc++ pdqsort by
    // ~3-5x at the anchor populations we see (50 k - 1 M+ at LONG/XL/XXL)
    // because it bypasses the comparison-based branch-predictor
    // pessimisation pdqsort hits when the ref_position prefix is
    // already monotonic — most of the work is just the 256-bucket
    // counting scan, which the M1 LSU pipelines well.
    //
    // A SIMD-vectorised bit-by-bit MSD radix variant (Bramas / Möller /
    // Keller, 2024 thesis) uses Highway's `CompressBlendedStore`. On
    // NEON-128 / Apple M1 the SIMD partition
    // is restricted to 2 u64 lanes per vector (vs. 8 on AVX-512) and
    // each NEON BlendedStore lowers to a 3-step Load+Blend+Store —
    // measured as 5–22 percentage points slower than this 8-bit MSD
    // radix sort on the benchmark's LONG/VLONG/XL/XXL buckets. The SIMD
    // variant becomes attractive again on AVX-512 hosts.
    sort_seed_hits(anchors_out);

    bioseq_log_lazy<logging_level::debug>([&anchors_out, &query_minimizers] {
        bioseq_log_progress<logging_level::debug>(
            "[minimap_port] Collected {} seed anchors from {} query minimizers"
            " | footprint={}MB avail={}MB",
            anchors_out.size(),
            query_minimizers.size(),
            bioseq::common::memory::process_footprint_bytes() >> 20,
            bioseq::common::memory::system_available_bytes() >> 20);
    });
}

// ============================================================================
// Stage 3: Chaining through the selected chaining module
// ============================================================================
// compute_chaining_score is imported from bioseq.chaining.core
// chain_anchors_scalar is imported from bioseq.chaining.scalar
// chain_anchors_simd is reached through the selected fixed-target table.
// chain_anchors_rmq is imported from bioseq.chaining.rmq

// ============================================================================
// Stage 4: Convert chains to alignment regions (mm_gen_regs equivalent)
// ============================================================================

/**
 * @brief Compute seeded match length and block length for a region.
 *
 * Equivalent to mm_cal_fuzzy_len() in hit.c.
 *
 * match_length: approximate number of matching bases in seeded regions
 * block_length: approximate total aligned length in seeded regions
 */
void compute_fuzzy_lengths(alignment_region& region, std::span<const anchor> anchors) {
    region.match_length = 0;
    region.block_length = 0;
    if (region.seed_count <= 0) {
        return;
    }

    const int32_t anchor_begin = region.anchor_start;
    // Accumulate in signed int64 (gaps are bounded position *deltas*, never
    // absolute positions) then store into the sequence_size_t fields.
    std::int64_t match_length = static_cast<int32_t>(anchors[anchor_begin].query_span);
    std::int64_t block_length = match_length;

    for (int32_t anchor_idx = anchor_begin + 1; anchor_idx < anchor_begin + region.seed_count;
         ++anchor_idx) {
        const int32_t span = static_cast<int32_t>(anchors[anchor_idx].query_span);
        const int32_t target_gap = static_cast<int32_t>(anchors[anchor_idx].ref_position -
                                                        anchors[anchor_idx - 1].ref_position);
        const int32_t query_gap = static_cast<int32_t>(anchors[anchor_idx].query_position -
                                                       anchors[anchor_idx - 1].query_position);
        const int32_t max_gap = (target_gap > query_gap) ? target_gap : query_gap;
        block_length += max_gap;
        const bool both_gaps_exceed_span = (target_gap > span && query_gap > span);
        const int32_t min_gap = (target_gap < query_gap) ? target_gap : query_gap;
        const int32_t match_increment = both_gaps_exceed_span ? span : min_gap;
        match_length += match_increment;
    }
    region.match_length = static_cast<sequence_size_t>(match_length);
    region.block_length = static_cast<sequence_size_t>(block_length);
}

/**
 * @brief Set alignment coordinates from seed positions.
 *
 * Equivalent to mm_reg_set_coor() in hit.c.
 * Derives query_start, query_end, ref_start, ref_end from first and last anchor.
 */
void set_region_coordinates(alignment_region& region,
                            std::span<const anchor> anchors,
                            sequence_size_t query_len) {
    if (region.seed_count <= 0) {
        return;
    }
    const int32_t anchor_begin = region.anchor_start;
    const anchor& first_anchor = anchors[anchor_begin];
    const anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

    region.is_reverse = first_anchor.is_reverse != 0;
    region.reference_id = 0; // : single-target codebase, reference_id always 0

    // Reference coordinates. The absolute positions are full 64-bit; the
    // intermediate `position + 1 - span` can be negative, so it is computed in
    // signed int64 and clamped before being stored in the sequence_size_t field.
    const std::int64_t first_span = static_cast<int32_t>(first_anchor.query_span);
    std::int64_t ref_start = static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
    const std::int64_t ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
    if (ref_start < 0) {
        ref_start = 0;
    }
    region.ref_start = static_cast<sequence_size_t>(ref_start);
    region.ref_end = static_cast<sequence_size_t>(ref_end);

    // Query coordinates
    std::int64_t raw_query_start =
        static_cast<std::int64_t>(first_anchor.query_position) + 1 - first_span;
    const std::int64_t raw_query_end = static_cast<std::int64_t>(last_anchor.query_position) + 1;
    if (raw_query_start < 0) {
        raw_query_start = 0;
    }

    if (!region.is_reverse) {
        region.query_start = static_cast<sequence_size_t>(raw_query_start);
        region.query_end = static_cast<sequence_size_t>(raw_query_end);
    } else {
        const std::int64_t qlen = static_cast<std::int64_t>(query_len);
        region.query_start = static_cast<sequence_size_t>(qlen - raw_query_end);
        region.query_end = static_cast<sequence_size_t>(qlen - raw_query_start);
    }

    compute_fuzzy_lengths(region, anchors);
}

/**
 * @brief Convert chains to alignment regions.
 *
 * Equivalent to mm_gen_regs() in hit.c:54-90.
 *
 * Sorts chains by score (descending) and creates alignment_region records
 * with coordinates derived from the anchor positions.
 *
 * @param chains       Chains (sorted in place by score descending here).
 * @param anchors      Reordered anchor span — `chains[i].anchor_start`
 *                      indexes into this span.
 * @param query_len    Length of the query sequence (used to flip query
 *                      coordinates for reverse-strand chains).
 * @param regions_out  Output slab — one alignment_region per chain is constructed
 *                      via placement-new, so the slab destructor releases
 *                      `region.cigar` during unwind. Must be a fresh arena_slab.
 *                      The kernel calls `reserve_for_placement_new` and
 *                      `construct_at_back` once per chain.
 */
void generate_regions(std::span<chain> chains,
                      std::span<const anchor> anchors,
                      sequence_size_t query_len,
                      ::bioseq::chaining::arena_slab<alignment_region>& regions_out) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_gen_regs");
    auto _scope = bioseq::instrumentation::region(_desc);
    if (chains.empty()) {
        return;
    }

    /// Order chains by descending score without capturing local state.
    struct chain_score_descending {
        [[nodiscard, gnu::always_inline]] inline bool
        operator()(const chain& left, const chain& right) const noexcept {
            return left.score > right.score;
        }
    };
    std::sort(chains.begin(), chains.end(), chain_score_descending{});

    regions_out.reserve_for_placement_new(chains.size());
    for (size_t region_idx = 0; region_idx < chains.size(); ++region_idx) {
        alignment_region& region = regions_out.construct_at_back(alignment_region{
            .region_id = static_cast<int32_t>(region_idx),
            .parent = -1,
            .score = chains[region_idx].score,
            .original_score = chains[region_idx].score,
            .seed_count = chains[region_idx].anchor_count,
            .anchor_start = chains[region_idx].anchor_start,
            .divergence = -1.0f,
        });
        set_region_coordinates(region, anchors, query_len);
    }

    bioseq_log_lazy<logging_level::debug>([&regions_out] {
        bioseq_log_progress<logging_level::debug>(
            "[minimap_port] Generated {} alignment regions | footprint={}MB avail={}MB",
            regions_out.size(),
            bioseq::common::memory::process_footprint_bytes() >> 20,
            bioseq::common::memory::system_available_bytes() >> 20);
    });
}

// ============================================================================
// Stage 5: Primary/secondary classification (mm_set_parent equivalent)
// ============================================================================

/**
 * @brief Classify alignments as primary or secondary.
 *
 * Equivalent to mm_set_parent() in hit.c:127-188.
 *
 * An alignment is secondary if it overlaps significantly with a primary
 * on the query AND has insufficient unique (uncovered) sequence.
 *
 * Criterion: (overlap/min_interval) - (uncov/max_interval) > mask_level
 *            AND uncov <= mask_len
 */
void classify_primary_secondary(std::span<alignment_region> regions,
                                float mask_level,
                                int32_t mask_len,
                                int32_t sub_diff) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_set_parent");
    auto _scope = bioseq::instrumentation::region(_desc);
    const size_t region_count = regions.size();
    if (region_count == 0) {
        return;
    }

    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    // primary_indices holds indices of primary alignments. Reserve up
    // front so the first push_back doesn't trigger a 1→2→4→… growth
    // chain through `region_count` worth of pushes when the chains
    // happen to be all primary. Sized at region_count (the upper bound
    // — every region is primary in the degenerate case).
    std::vector<size_t> primary_indices;
    primary_indices.reserve(region_count);
    primary_indices.push_back(0);
    regions[0].parent = 0;

    // coverage_intervals is cleared+refilled on every secondary check,
    // capped at primary_indices.size() entries. Pre-reserve at
    // region_count too so the inner loop never triggers realloc.
    std::vector<uint64_t> coverage_intervals;
    coverage_intervals.reserve(region_count);

    for (size_t region_idx = 1; region_idx < region_count; ++region_idx) {
        alignment_region& current_region = regions[region_idx];
        const int32_t current_query_start = current_region.query_start;
        const int32_t current_query_end = current_region.query_end;
        int32_t uncovered_length = 0;

        // Compute coverage from existing primaries
        coverage_intervals.clear();
        for (const size_t primary_idx : primary_indices) {
            const alignment_region& primary_region = regions[primary_idx];
            int32_t primary_query_start = primary_region.query_start;
            int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start ||
                primary_query_start >= current_query_end) {
                continue;
            }
            primary_query_start = std::max(primary_query_start, current_query_start);
            primary_query_end = std::min(primary_query_end, current_query_end);
            coverage_intervals.push_back((static_cast<uint64_t>(primary_query_start) << 32) |
                                         static_cast<uint32_t>(primary_query_end));
        }

        if (!coverage_intervals.empty()) {
            std::sort(coverage_intervals.begin(), coverage_intervals.end());
            int32_t coverage_pos = current_query_start;
            for (const uint64_t interval : coverage_intervals) {
                const int32_t interval_start = static_cast<int32_t>(interval >> 32);
                const int32_t interval_end = static_cast<int32_t>(interval);
                if (interval_start > coverage_pos) {
                    uncovered_length += interval_start - coverage_pos;
                }
                coverage_pos = std::max(coverage_pos, interval_end);
            }
            if (current_query_end > coverage_pos) {
                uncovered_length += current_query_end - coverage_pos;
            }
        }

        // Test against each primary
        bool is_secondary = false;
        for (const size_t primary_idx : primary_indices) {
            alignment_region& primary_region = regions[primary_idx];
            const int32_t primary_query_start = primary_region.query_start;
            const int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start ||
                primary_query_start >= current_query_end) {
                continue;
            }

            const int32_t min_span = std::min(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t max_span = std::max(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t overlap_length =
                std::max(0,
                         std::min(current_query_end, primary_query_end) -
                             std::max(current_query_start, primary_query_start));

            if (min_span > 0 && max_span > 0 &&
                (static_cast<float>(overlap_length) / min_span -
                 static_cast<float>(uncovered_length) / max_span) > mask_level &&
                uncovered_length <= mask_len) {
                const int32_t resolved_parent =
                    (primary_region.parent >= 0) ? primary_region.parent : primary_region.region_id;
                current_region.parent = resolved_parent;
                primary_region.secondary_score =
                    std::max(primary_region.secondary_score, current_region.score);
                // Update secondary_dp_max for MAPQ (equivalent to hit.c:176)
                if (current_region.has_alignment && primary_region.has_alignment &&
                    (primary_region.reference_id != current_region.reference_id ||
                     primary_region.ref_start != current_region.ref_start ||
                     primary_region.ref_end != current_region.ref_end ||
                     overlap_length != min_span)) {
                    primary_region.secondary_dp_max =
                        std::max(primary_region.secondary_dp_max, current_region.dp_max_score);
                    if (primary_region.dp_max_score - current_region.dp_max_score <= sub_diff) {
                        ++primary_region.secondary_count;
                    }
                } else if (current_region.seed_count >= primary_region.seed_count) {
                    ++primary_region.secondary_count;
                }
                is_secondary = true;
                break;
            }
        }

        if (!is_secondary) {
            primary_indices.push_back(region_idx);
            current_region.parent = static_cast<int32_t>(region_idx);
            current_region.secondary_count = 0;
        }
    }

    bioseq_log<logging_level::debug>("[minimap_port] Classification: {} primary, {} secondary",
                                     primary_indices.size(),
                                     region_count - primary_indices.size());
}

// ============================================================================
// Stage 6: Filter secondary mappings (mm_select_sub equivalent)
// ============================================================================

/**
 * @brief Filter secondary alignments by score ratio.
 *
 * Equivalent to mm_select_sub() in hit.c:257-277.
 *
 * Keep a secondary if:
 *   - score >= parent.score * pri_ratio, OR
 *   - score + min_diff >= parent.score
 *   - up to best_n secondaries total
 */
[[nodiscard]] std::size_t filter_secondary_regions(std::span<alignment_region> regions,
                                                   float pri_ratio,
                                                   int32_t min_diff,
                                                   int32_t best_n) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_select_sub");
    auto _scope = bioseq::instrumentation::region(_desc);
    if (regions.empty() || pri_ratio <= 0.0f) {
        return regions.size();
    }

    // Use index-based filtering to avoid reading from moved-from objects.
    // Mark each region as kept (true) or removed (false), then compact.
    const size_t region_count = regions.size();
    size_t secondary_kept_count = 0;
    std::vector<bool> keep(region_count, false);

    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        const auto& region = regions[region_idx];
        const int32_t parent_idx = region.parent;
        if (parent_idx == region.region_id) {
            // Primary — always keep
            keep[region_idx] = true;
        } else if (parent_idx >= 0 && parent_idx < static_cast<int32_t>(region_count)) {
            const auto& parent_region = regions[parent_idx];
            if ((region.score >= static_cast<int32_t>(parent_region.score * pri_ratio) ||
                 region.score + min_diff >= parent_region.score) &&
                static_cast<int32_t>(secondary_kept_count) < best_n) {
                // Not identical coordinates
                const bool is_duplicate = region.query_start == parent_region.query_start &&
                                          region.query_end == parent_region.query_end &&
                                          region.reference_id == parent_region.reference_id &&
                                          region.ref_start == parent_region.ref_start &&
                                          region.ref_end == parent_region.ref_end;
                if (!is_duplicate) {
                    keep[region_idx] = true;
                    ++secondary_kept_count;
                }
            }
        }
    }

    // Compact: move kept regions to front.  Span has no resize(); the caller
    // observes the new effective size via the returned value.  Regions past
    // `write_pos` are moved-from (empty CIGAR vector) — their destructors run
    // when the owning storage (arena slab or vector) unwinds.
    size_t write_pos = 0;
    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        if (keep[region_idx]) {
            if (write_pos < region_idx) {
                regions[write_pos] = std::move(regions[region_idx]);
            }
            ++write_pos;
        }
    }

    // Re-index the surviving prefix.
    for (size_t region_idx = 0; region_idx < write_pos; ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    bioseq_log<logging_level::debug>("[minimap_port] After secondary filtering: {} regions remain",
                                     write_pos);
    return write_pos;
}

} // namespace bioseq::simd
