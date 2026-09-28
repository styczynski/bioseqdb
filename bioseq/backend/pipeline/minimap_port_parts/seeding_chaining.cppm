/**
 * @file pipeline/minimap_port_parts/seeding_chaining.cppm
 * @brief Finds seed hits and turns chains into selected alignment regions.
 */
module;

#include <new>

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

// Seed batches use the anchor and arena types from bioseq.chaining.core.

/** @brief A  query ordinal and its anchor range within a seed batch. */
struct canonical_seed_job_span {
    std::uint32_t ordinal;
    std::size_t offset;
    std::size_t count;
};

/**
 * @brief Holds host anchors and the ranges belonging to each query.
 *
 * All seed backends produce this batch format Keep the chaining arena and
 * its memory reservation alive until the batch and all borrowed ranges are
 * finished. The batch can be moved, but cannot be copied.
 */
struct canonical_seed_batch {
    bioseq::chaining::arena_slab<bioseq::chaining::anchor> anchors;
    std::vector<canonical_seed_job_span> jobs;
};

/** Check that ordered query jobs cover every anchor exactly once. */
[[nodiscard]] inline bool canonical_seed_jobs_match_submission(
    const canonical_seed_batch& batch,
    std::span<const std::uint32_t> submitted_ordinals) noexcept {
    if (batch.jobs.size() != submitted_ordinals.size()) {
        return false;
    }
    std::size_t prefix = 0u;
    for (std::size_t index = 0u; index < batch.jobs.size(); ++index) {
        const auto& job = batch.jobs[index];
        if (job.ordinal != submitted_ordinals[index] ||
            (index != 0u && job.ordinal <= batch.jobs[index - 1u].ordinal) ||
            job.offset != prefix || prefix > batch.anchors.size() ||
            job.count > batch.anchors.size() - prefix) {
            return false;
        }
        prefix += job.count;
    }
    return prefix == batch.anchors.size();
}

// Alignment regions and chain scores use helpers from  their own modules.

// Target-index setup is handled by the indexing modules.

// Lookup accepts the index types described by minimizer_lookup_index.

// Look up query minimizers in the target index to collect seed hits.

/**
 * Append anchors for one query minimizer's target hits. Decode each yarget
 * position and strand, then choose the matching query-strand coordinate.
 * Read 32-bit postings directly and widen each value only while building
 * its anchor, so, no temporary 64-bit position array is needed.
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

/// Sort anchors by the shared strand, reference, and  query ordering.
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
 * @brief Find matching target minimizers and append anchors for chaining.
 *
 * Skip hashes with no hits or a count exceeding max_occ. Remaining hits
 * supply reference positions and strand-adjusted query positions. Sort the
 * output slab before returning.
 *
 * @param query_len Query length in bases.
 * @param max_occ Nonnegative occurrence limit.
 * @param kmer_size Span assigned to each anchor.
 * @param anchors_out Live slab in the caller's chaining arena
 */
template <class p_minimizer, minimizer_lookup_index p_index>
void collect_seed_hits(std::span<const p_minimizer> query_minimizers,
                       const p_index& target_index,
                       sequence_size_t query_len,
                       std::int32_t max_occ,
                       std::int32_t kmer_size,
                       ::bioseq::chaining::arena_slab<anchor>& anchors_out) {
    static const auto* _seed_hits_desc =
        bioseq::instrumentation::register_static_region("collect_seed_hits");
    auto _seed_hits_scope = bioseq::instrumentation::region(_seed_hits_desc);
    // Reserve an initial capacity for direct callers. An already-large slab
    // keeps its allocation, and push_back can grow it if more hits are found.
    anchors_out.reserve(query_minimizers.size() * 2);

    // Time the whole  lookup loop to keep per-minimizer profiler overhead low.
    {
        static const auto* _idx_lookup_desc =
            bioseq::instrumentation::register_static_region("target_index_lookup");
        auto _idx_lookup_scope = bioseq::instrumentation::region(_idx_lookup_desc);
        // Every seed in this call uses the configured k-mer span.
        const std::uint32_t query_span_value = static_cast<std::uint32_t>(kmer_size);
        // Precompute the fixed part of the reverse-complement end coordinate.
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

            // Keep the encoded position and strand in 64 bits. A position at or
            // above 2^31 needs an extra bit once the stranq is packed with it.
            const std::uint64_t query_pos_encoded = query_minimizer.pos_strand_encoded();
            // The query strand is shared by every target hit for this minimizer.
            const std::uint32_t query_strand_bit = static_cast<std::uint32_t>(query_pos_encoded & 1ULL);
            const sequence_size_t forward_query_pos =
                static_cast<sequence_size_t>(query_pos_encoded >> 1);
            // Compute both  strand coordinates once for all hits of this minimizer.
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

    // Use the shared anchor sorter so every seeder gives chaining the same
    // order. It groups strand/reference positions first, then resolves ties
    // by the query key and full query position.
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

// Scalar and RMQ chaining come from their chaining modules. SIMD
// chaining uses the function table selected for the current CPU target.

// Convert chains into alignment regions.

/**
 * @brief Estimate matched and aligned lengths from chain seeds.
 *
 * Count seed coverage as approximate matches and the larger query/reference
 * advances as block, length. These estimates are available before  base alignment
 */
void compute_fuzzy_lengths(alignment_region& region, std::span<const anchor> anchors) {
    region.match_length = 0;
    region.block_length = 0;
    if (region.seed_count <= 0) {
        return;
    }

    const std::int32_t anchor_begin = region.anchor_start;
    // Accumulate seed spans and the bounded gaps between adjacent anchors.
    std::int64_t match_length = static_cast<std::int32_t>(anchors[anchor_begin].query_span);
    std::int64_t block_length = match_length;

    for (std::int32_t anchor_idx = anchor_begin + 1; anchor_idx < anchor_begin + region.seed_count;
         ++anchor_idx) {
        const std::int32_t span = static_cast<std::int32_t>(anchors[anchor_idx].query_span);
        const std::int32_t target_gap = static_cast<std::int32_t>(anchors[anchor_idx].ref_position -
                                                        anchors[anchor_idx - 1].ref_position);
        const std::int32_t query_gap = static_cast<std::int32_t>(anchors[anchor_idx].query_position -
                                                       anchors[anchor_idx - 1].query_position);
        const std::int32_t max_gap = (target_gap > query_gap) ? target_gap : query_gap;
        block_length += max_gap;
        const bool both_gaps_exceed_span = (target_gap > span && query_gap > span);
        const std::int32_t min_gap = (target_gap < query_gap) ? target_gap : query_gap;
        const std::int32_t match_increment = both_gaps_exceed_span ? span : min_gap;
        match_length += match_increment;
    }
    region.match_length = static_cast<sequence_size_t>(match_length);
    region.block_length = static_cast<sequence_size_t>(block_length);
}

/**
 * @brief Derive a region's bounds and estimated lengths from its outer seeds.
 *
 * Store half-open reference and query ranges. Reverse-strand query bounds
 * are converted back to positions on the forward quemy.
 */
void set_region_coordinates(alignment_region& region,
                            std::span<const anchor> anchors,
                            sequence_size_t query_len) {
    if (region.seed_count <= 0) {
        return;
    }
    const std::int32_t anchor_begin = region.anchor_start;
    const anchor& first_anchor = anchors[anchor_begin];
    const anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

    region.is_reverse = first_anchor.is_reverse != 0;
    region.reference_id = 0; // Each pipeline call searches one target.

    // Use signed arithmetic while extending the first seed to its start,
    // then clamp at zero before storing the unsigned reference bound.
    const std::int64_t first_span = static_cast<std::int32_t>(first_anchor.query_span);
    std::int64_t ref_start = static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
    const std::int64_t ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
    if (ref_start < 0) {
        ref_start = 0;
    }
    region.ref_start = static_cast<sequence_size_t>(ref_start);
    region.ref_end = static_cast<sequence_size_t>(ref_end);

    // Derive query bounds in  chain orientation before converting the strand.
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
 * @brief Create one region per chain, ordered by descending chain score.
 *
 * Sort chains in place and use their anchor ranges to set each region's
 * coordinates. query_len converts reverse-strand query positions.
 * regions_out must be a fresh arena slab. Construct its regions individually
 * so their CIGAR storage is destroyed when the slab is released.
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto tail_storage_plan = derive_alignment_tail_storage_plan(chains.size());
    if (!tail_storage_plan) {
        throw std::length_error{"region count exceeds the alignment tail domain"};
    }
#endif

    /// Order chains from highest score to lowest.
    struct chain_score_descending {
        [[nodiscard, gnu::always_inline]] inline bool
        operator()(const chain& left, const chain& right) const noexcept {
            return left.score > right.score;
        }
    };
    std::sort(chains.begin(), chains.end(), chain_score_descending{});

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    regions_out.reserve_for_placement_new(tail_storage_plan->region_count);
#else
    regions_out.reserve_for_placement_new(chains.size());
#endif
    for (std::size_t region_idx = 0; region_idx < chains.size(); ++region_idx) {
        alignment_region& region = regions_out.construct_at_back(alignment_region{
            .region_id = static_cast<std::int32_t>(region_idx),
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

// Classify primary and secondary regions.

/**
 * @brief Assign secondary regions to overlapping primaries.
 *
 * Process regions in  priority order. A region becomes secondary when its
 * adjusted query overlap exceeds mask_level and its uncovered length is at
 * most mask_len. Otherwise it becomes another primary. Record competing
 * scores and counts on the primary for mapping-quality estimation.
 */
void classify_primary_secondary(std::span<alignment_region> regions,
                                float mask_level,
                                std::int32_t mask_len,
                                std::int32_t sub_diff) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_set_parent");
    auto _scope = bioseq::instrumentation::region(_desc);
    const std::size_t region_count = regions.size();
    if (region_count == 0) {
        return;
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto tail_storage_plan = derive_alignment_tail_storage_plan(region_count);
    if (!tail_storage_plan) {
        throw std::length_error{"classification region count is not representable"};
    }
#endif

    for (std::size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        regions[region_idx].region_id = static_cast<std::int32_t>(region_idx);
    }

    // Every region may become primary Reserve that upper bound, nce.
    alignment_result_vector<std::size_t> primary_indices;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    primary_indices.set_logical_bounds(
        tail_storage_plan->primary_index_capacity,
        tail_storage_plan->primary_index_capacity);
    primary_indices.reserve(tail_storage_plan->primary_index_capacity);
#else
    primary_indices.reserve(region_count);
#endif
    primary_indices.push_back(0);
    regions[0].parent = 0;

    // Reuse enough interval storage to compare against every primary.
    alignment_result_vector<std::uint64_t> coverage_intervals;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    coverage_intervals.set_logical_bounds(
        tail_storage_plan->coverage_interval_capacity,
        tail_storage_plan->coverage_interval_capacity);
    coverage_intervals.reserve(tail_storage_plan->coverage_interval_capacity);
#else
    coverage_intervals.reserve(region_count);
#endif

    for (std::size_t region_idx = 1; region_idx < region_count; ++region_idx) {
        alignment_region& current_region = regions[region_idx];
        const std::int32_t current_query_start = current_region.query_start;
        const std::int32_t current_query_end = current_region.query_end;
        std::int32_t uncovered_length = 0;

        // Collect the parts of this query range covered by existing primaries.
        coverage_intervals.clear();
        for (const std::size_t primary_idx : primary_indices) {
            const alignment_region& primary_region = regions[primary_idx];
            std::int32_t primary_query_start = primary_region.query_start;
            std::int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start ||
                primary_query_start >= current_query_end) {
                continue;
            }
            primary_query_start = std::max(primary_query_start, current_query_start);
            primary_query_end = std::min(primary_query_end, current_query_end);
            coverage_intervals.push_back((static_cast<std::uint64_t>(primary_query_start) << 32) |
                                         static_cast<std::uint32_t>(primary_query_end));
        }

        if (!coverage_intervals.empty()) {
            std::sort(coverage_intervals.begin(), coverage_intervals.end());
            std::int32_t coverage_pos = current_query_start;
            for (const std::uint64_t interval : coverage_intervals) {
                const std::int32_t interval_start = static_cast<std::int32_t>(interval >> 32);
                const std::int32_t interval_end = static_cast<std::int32_t>(interval);
                if (interval_start > coverage_pos) {
                    uncovered_length += interval_start - coverage_pos;
                }
                coverage_pos = std::max(coverage_pos, interval_end);
            }
            if (current_query_end > coverage_pos) {
                uncovered_length += current_query_end - coverage_pos;
            }
        }

        // Find the first primary that meets the overlap  and uncovered-length rules.
        bool is_secondary = false;
        for (const std::size_t primary_idx : primary_indices) {
            alignment_region& primary_region = regions[primary_idx];
            const std::int32_t primary_query_start = primary_region.query_start;
            const std::int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start ||
                primary_query_start >= current_query_end) {
                continue;
            }

            const std::int32_t min_span = std::min(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const std::int32_t max_span = std::max(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const std::int32_t overlap_length =
                std::max(0,
                         std::min(current_query_end, primary_query_end) -
                             std::max(current_query_start, primary_query_start));

            if (min_span > 0 && max_span > 0 &&
                (static_cast<float>(overlap_length) / min_span -
                 static_cast<float>(uncovered_length) / max_span) > mask_level &&
                uncovered_length <= mask_len) {
                const std::int32_t resolved_parent =
                    (primary_region.parent >= 0) ? primary_region.parent : primary_region.region_id;
                current_region.parent = resolved_parent;
                primary_region.secondary_score =
                    std::max(primary_region.secondary_score, current_region.score);
                // Record the strongest competing alignment for mapping-quality estimation.
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
            current_region.parent = static_cast<std::int32_t>(region_idx);
            current_region.secondary_count = 0;
        }
    }

    bioseq_log<logging_level::debug>("[minimap_port] Classification: {} primary, {} secondary",
                                     primary_indices.size(),
                                     region_count - primary_indices.size());
}

// Keep the strongest secondary mappings.

/**
 * @brief Keep primaries and up to best_n qualifying secondaries.
 *
 * A secondary qualifies by its score ratio or by being within min_diff of
 * its parent's score. Remove secondaries with identical parent coordinates.
 * Compact survivors to the front and return their count. A nonpositive
 * pri_ratio disables filtering.
 */
[[nodiscard]] std::size_t filter_secondary_regions(std::span<alignment_region> regions,
                                                   float pri_ratio,
                                                   std::int32_t min_diff,
                                                   std::int32_t best_n) {
    static const auto* _desc = bioseq::instrumentation::register_static_region("mm_select_sub");
    auto _scope = bioseq::instrumentation::region(_desc);
    if (regions.empty() || pri_ratio <= 0.0f) {
        return regions.size();
    }

    // Decide which regions survive before moving any of them, so parent
    // lookups still read the original records.
    const std::size_t region_count = regions.size();
    std::size_t secondary_kept_count = 0;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto tail_storage_plan = derive_alignment_tail_storage_plan(region_count);
    if (!tail_storage_plan) {
        throw std::length_error{"filter region count is not representable"};
    }
    alignment_result_vector<std::uint8_t> keep;
    keep.set_logical_bounds(
        tail_storage_plan->filter_marker_count,
        tail_storage_plan->filter_marker_count);
    keep.resize(tail_storage_plan->filter_marker_count);
#else
    std::vector<bool> keep(region_count, false);
#endif

    for (std::size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        const auto& region = regions[region_idx];
        const std::int32_t parent_idx = region.parent;
        if (parent_idx == region.region_id) {
            // Keep every  primary.
            keep[region_idx] = true;
        } else if (parent_idx >= 0 && parent_idx < static_cast<std::int32_t>(region_count)) {
            const auto& parent_region = regions[parent_idx];
            if ((region.score >= static_cast<std::int32_t>(parent_region.score * pri_ratio) ||
                 region.score + min_diff >= parent_region.score) &&
                static_cast<std::int32_t>(secondary_kept_count) < best_n) {
                // Suppress a secondary with the same query and target bounds as its parent.
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

    // Move survivors to the front and return the shorter range to the caller.
    // The owning slab still destroys every constructed region, including
    // rejected records and records whose CIGAR was moved to the front.
    std::size_t write_pos = 0;
    for (std::size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        if (keep[region_idx]) {
            if (write_pos < region_idx) {
                regions[write_pos] = std::move(regions[region_idx]);
            }
            ++write_pos;
        }
    }

    // Assign consecutive IDs to surviviug regions.
    for (std::size_t region_idx = 0; region_idx < write_pos; ++region_idx) {
        regions[region_idx].region_id = static_cast<std::int32_t>(region_idx);
    }

    bioseq_log<logging_level::debug>("[minimap_port] After secondary filtering: {} regions remain",
                                     write_pos);
    return write_pos;
}

} // namespace bioseq::simd
