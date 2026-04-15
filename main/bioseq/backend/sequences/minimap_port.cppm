/**
 * @file minimap_port.cppm
 * @brief C++20 port of the minimap2 mapping pipeline.
 *
 * This module implements the core minimap2 algorithm starting AFTER minimizer
 * collection (sketch generation). It takes pre-computed minimizers and produces
 * PAF-like alignment results.
 *
 * High-level flow:
 *   1. Build a simple hash index from target minimizers
 *   2. Collect seed hits: look up query minimizers in the target index
 *   3. Chain seeds using dynamic programming (mg_lchain_dp equivalent)
 *   4. Convert chains to alignment regions (mm_gen_regs equivalent)
 *   5. Classify primary/secondary mappings (mm_set_parent equivalent)
 *   6. Filter secondary mappings (mm_select_sub equivalent)
 *   7. Perform base-level alignment using dual-affine gap DP (KSW2 equivalent)
 *   8. Compute mapping quality (mm_set_mapq equivalent)
 *   9. Return results as MatchResult (PAF-like records)
 *
 * The implementation follows the original minimap2 C code as documented in
 * MINIMAP2_DETAILS_1.md, translating to modern C++20 with:
 *   - std::vector instead of C arrays
 *   - References instead of raw pointers
 *   - static_cast instead of C-style casts
 *   - Move semantics where beneficial
 *   - constexpr where possible
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <cstdint>
#include <cmath>
#include <cstring>
#include <bit>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>
#include <limits>
#include <unordered_map>
#include <optional>
#include <span>
#include <string>

export module bioseq_sequences.minimap_port;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_cigar;
import alignment_hwy;
import alignment_hwy_utils;
import block_aligner_hwy;
import bioseq_sequences.chaining.utils;
import bioseq_sequences.chaining.scalar;
import bioseq_sequences.chaining.simd;
import bioseq_sequences.chaining.rmq;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Import chaining types from the shared chaining module
using bioseq::chaining::Anchor;
using bioseq::chaining::AnchorRefFields;
using bioseq::chaining::AnchorQueryFields;
using bioseq::chaining::Chain;
using bioseq::chaining::ChainingMode;
using bioseq::chaining::ChainingParams;
using bioseq::chaining::compute_chaining_score;
using bioseq::chaining::chain_anchors_scalar;
using bioseq::chaining::chain_anchors_rmq;
using bioseq::chaining::backtrack_chains;

// ============================================================================
// Constants — mirrors minimap2 defaults from mm_mapopt_t
// ============================================================================

struct Minimap2Params {
    int32_t kmer_size      = 15;     ///< k-mer size
    int32_t window_size    = 10;     ///< minimizer window
    int32_t max_gap        = 5000;   ///< max reference gap in chaining
    int32_t max_gap_ref    = -1;     ///< explicit ref gap (-1 = use max_gap)
    int32_t bandwidth      = 500;    ///< bandwidth for chaining & alignment
    int32_t max_chain_skip = 25;     ///< max skip for early termination in DP
    int32_t max_chain_iter = 5000;   ///< max predecessors examined per anchor (map.c)
    int32_t min_seed_count = 3;      ///< min seeds per chain
    int32_t min_chain_score= 40;     ///< min chain score
    float   chain_gap_scale= 0.8f;   ///< gap penalty scale (options.c default)
    float   chain_skip_scale=0.0f;   ///< skip penalty scale (options.c default)
    float   mask_level     = 0.5f;   ///< primary/secondary overlap threshold
    int32_t mask_len       = std::numeric_limits<int32_t>::max(); ///< max uncovered length (INT_MAX in options.c)
    float   pri_ratio      = 0.8f;   ///< secondary score ratio threshold
    int32_t best_n         = 5;      ///< max secondary alignments to keep

    // Alignment scoring — matches mm_mapopt_init() in options.c
    int32_t match_score    = 2;      ///< match score (a)
    int32_t mismatch_pen   = 4;      ///< mismatch penalty (b)
    int32_t gap_open1      = 4;      ///< short gap open (q)
    int32_t gap_ext1       = 2;      ///< short gap extend (e)
    int32_t gap_open2      = 24;     ///< long gap open (q2)
    int32_t gap_ext2       = 1;      ///< long gap extend (e2)
    int32_t sc_ambi        = 1;      ///< penalty for ambiguous base (N)
    int32_t zdrop          = 400;    ///< z-drop threshold
    int32_t end_bonus      = -1;     ///< bonus for reaching sequence ends (-1 in options.c)

    int32_t max_occ        = 500;    ///< max seed occurrence to keep
    int32_t mid_occ        = 500;    ///< mid-level seed occurrence threshold

    /// When true, perform base-level alignment via KSW2 and produce CIGAR.
    /// Equivalent to MM_F_CIGAR flag in minimap2. By default we use -c in build.sh
    bool    do_cigar       = true;

    /// When true, use block aligner instead of KSW2 for gap-filling alignment.
    /// Block aligner supports only single-affine gaps; extensions still use KSW2.
    bool    use_block_aligner = true;
    size_t  block_min_size    = 32;
    size_t  block_max_size    = 256;

    /// Algorithm selection for chaining. Default: SCALAR_DP for backwards compatibility.
    ChainingMode chaining_mode = ChainingMode::SCALAR_DP;
    int32_t bandwidth_long  = 0;  ///< Long-join bandwidth (0 = disabled)

    /// Build ChainingParams from this Minimap2Params.
    ///
    /// Extracts only the chaining-relevant subset and precomputes derived values.
    [[nodiscard]] ChainingParams to_chaining_params() const {
        ChainingParams cp;
        cp.kmer_size        = kmer_size;
        cp.max_gap          = max_gap;
        cp.bandwidth        = bandwidth;
        cp.max_chain_skip   = max_chain_skip;
        cp.max_chain_iter   = max_chain_iter;
        cp.min_seed_count   = min_seed_count;
        cp.min_chain_score  = min_chain_score;
        cp.chain_gap_scale  = chain_gap_scale;
        cp.chain_skip_scale = chain_skip_scale;
        cp.chaining_mode    = chaining_mode;
        cp.prepare();
        return cp;
    }
};

// CIGAR opcodes and CigarOp are imported from bioseq_cigar module.

/**
 * @brief PAF-like alignment result.
 *
 * Maps to the 12 mandatory PAF columns plus alignment details.
 */
struct MatchResult {
    // PAF mandatory columns
    int32_t query_len              = 0;    ///< col 2: query sequence length
    int32_t query_start            = 0;    ///< col 3: query start (0-based, closed)
    int32_t query_end              = 0;    ///< col 4: query end (0-based, open)
    bool    is_reverse             = false;///< col 5: '+' or '-'
    int32_t target_len             = 0;    ///< col 7: target sequence length
    int32_t target_start           = 0;    ///< col 8: target start on original strand
    int32_t target_end             = 0;    ///< col 9: target end on original strand
    int32_t num_matches            = 0;    ///< col 10: number of residue matches (mlen)
    int32_t block_length           = 0;    ///< col 11: alignment block length (blen)
    int32_t mapq                   = 0;    ///< col 12: mapping quality (0-255)

    // Additional alignment info
    int32_t chain_score            = 0;    ///< chaining score
    int32_t dp_score               = 0;    ///< DP alignment score
    int32_t dp_max_score           = 0;    ///< max scoring segment score (dp_max0 in original)
    int32_t seed_count             = 0;    ///< number of seeds in chain
    int32_t secondary_chain_score  = 0;    ///< best secondary chaining score against this primary
    float   divergence             = -1.f; ///< sequence divergence estimate
    bool    is_primary             = true; ///< primary mapping flag
    bool    has_cigar              = false;///< whether CIGAR alignment was performed
    std::vector<CigarOp> cigar;            ///< CIGAR string
};

// ============================================================================
// Internal data structures
// ============================================================================
// Anchor, AnchorRefFields, AnchorQueryFields, Chain are now imported from
// bioseq_sequences.chaining.utils via 'using' declarations above.

/**
 * @brief Alignment region — equivalent to mm_reg1_t.
 *
 * Produced after mm_gen_regs, enriched through the pipeline.
 */
struct AlignRegion {
    int32_t region_id       = 0;
    int32_t parent          = -1;   ///< parent id (-1 = unset, ==region_id => primary)
    int32_t score           = 0;    ///< chain score
    int32_t original_score  = 0;    ///< initial chain score (before merging)
    int32_t seed_count      = 0;    ///< number of seeds
    int32_t anchor_start    = 0;    ///< offset in anchor array

    int32_t query_start     = 0;    ///< query start position
    int32_t query_end       = 0;    ///< query end position
    int32_t ref_start       = 0;    ///< reference start position
    int32_t ref_end         = 0;    ///< reference end position
    int32_t reference_id    = 0;    ///< reference sequence id (0 in our case)
    bool    is_reverse      = false;///< reverse strand

    int32_t match_length    = 0;    ///< seeded exact match length
    int32_t block_length    = 0;    ///< seeded alignment block length
    int32_t secondary_score = 0;    ///< best secondary score against this primary
    int32_t secondary_count = 0;    ///< number of suboptimal mappings

    // DP alignment results (filled after alignment phase)
    int32_t dp_score            = 0;
    int32_t dp_max_score        = 0;    ///< max scoring segment
    int32_t secondary_dp_max    = 0;    ///< best secondary DP score
    bool    has_alignment       = false;///< whether DP alignment was performed
    float   divergence          = -1.f; ///< divergence estimate

    int32_t mapq            = 0;

    std::vector<CigarOp> cigar;
};

// fast_log2 is now in bioseq_sequences.chaining.utils

// ============================================================================
// Phase 1: Build target index from target minimizers
// ============================================================================

/**
 * @brief Simple hash map index for target minimizers.
 *
 * Maps minimizer hash -> vector of (position, strand) pairs in the target.
 * Equivalent to mm_idx_t's B[] bucket array + hash table, but simplified
 * for a single target sequence.
 *
 * Each stored position encodes: (rid << 32) | (pos << 1) | strand
 * For single-target, rid = 0.
 */
struct TargetIndex {
    std::unordered_map<uint64_t, std::vector<uint64_t>> table;
    int32_t target_len = 0;

    /**
     * @brief Build the index from target minimizer sketches.
     *
     * Equivalent to mm_idx_add + worker_post in index.c.
     * For each minimizer, we store its position in the hash table keyed
     * by the k-mer hash (upper 56 bits of MinimapSketchData.x).
     */
    void build(const std::vector<MinimapSketchData>& target_minimizers,
               int32_t target_length) {
        target_len = target_length;
        table.clear();
        table.reserve(target_minimizers.size());
        for (const auto& minimizer : target_minimizers) {
            const uint64_t hash_key = minimizer.kmer;
            // Encode position as: (rid << 32) | (last_pos << 1) | strand
            // rid = 0 for single target
            const uint64_t encoded_position =
                (static_cast<uint64_t>(minimizer.last_pos) << 1)
                | static_cast<uint64_t>(minimizer.strand);
            table[hash_key].push_back(encoded_position);
        }
        JustLog("[minimap_port] Built target index: {} distinct minimizers from {} total",
                table.size(), target_minimizers.size());
    }

    /**
     * @brief Look up a minimizer hash in the index.
     *
     * Equivalent to mm_idx_get() in index.c:96-113.
     *
     * @param kmer_hash The k-mer hash (56-bit value from MinimapSketchData.kmer)
     * @return Span of positions (empty if not found)
     */
    [[nodiscard]] std::span<const uint64_t> lookup(uint64_t kmer_hash) const {
        auto iter = table.find(kmer_hash);
        if (iter == table.end()) {
            return {};
        }
        return std::span<const uint64_t>(iter->second);
    }
};

// ============================================================================
// Phase 2: Collect seed hits — look up query minimizers in target index
// ============================================================================

/**
 * @brief Collect seed anchors by looking up query minimizers in the target index.
 *
 * Equivalent to collect_seed_hits() in map.c:518-554.
 *
 * For each query minimizer, look up its hash in the target index. For each
 * hit, create an anchor encoding both reference and query positions.
 *
 * Anchor encoding (same as minimap2):
 *   ref_bits: is_reverse<<63 | reference_id<<32 | ref_position
 *   query_bits: query_span<<32 | query_position
 *
 * Where for forward strand: ref_position = target_pos, query_position = query_pos
 * For reverse strand: set is_reverse bit, adjust query_position for reverse complement.
 *
 * @param query_minimizers Pre-computed query minimizers
 * @param target_index     Target index to search
 * @param query_len        Length of query sequence
 * @param max_occ          Max occurrences to keep (filter high-frequency seeds)
 * @return Sorted vector of anchors
 */
[[nodiscard]] std::vector<Anchor> collect_seed_hits(
    const std::vector<MinimapSketchData>& query_minimizers,
    const TargetIndex& target_index,
    int32_t query_len,
    int32_t max_occ)
{
    std::vector<Anchor> anchors;
    anchors.reserve(query_minimizers.size() * 2);

    for (const auto& query_minimizer : query_minimizers) {
        const auto target_hits = target_index.lookup(query_minimizer.kmer);
        if (target_hits.empty() || static_cast<int32_t>(target_hits.size()) > max_occ) {
            continue;
        }

        const uint32_t query_pos_encoded = static_cast<uint32_t>(
            (query_minimizer.last_pos << 1) | query_minimizer.strand);
        const uint32_t query_span_value = static_cast<uint32_t>(query_minimizer.span);

        for (const uint64_t target_hit : target_hits) {
            const int32_t ref_position = static_cast<int32_t>(
                static_cast<uint32_t>(target_hit) >> 1);
            const uint32_t target_reference_id = static_cast<uint32_t>(target_hit >> 32);

            Anchor anchor;
            const bool strands_match = ((target_hit & 1) == (query_pos_encoded & 1));

            if (strands_match) {
                // Forward strand hit — set reference fields via bitfields
                anchor.ref.ref_position = static_cast<uint32_t>(ref_position);
                anchor.ref.reference_id = target_reference_id;
                anchor.ref.is_reverse = 0;
                anchor.query.query_position = query_pos_encoded >> 1;
                anchor.query.query_span = query_span_value;
                anchor.query.flags = 0;
                anchor.query.segment_id = 0;
            } else {
                // Reverse strand hit
                anchor.ref.ref_position = static_cast<uint32_t>(ref_position);
                anchor.ref.reference_id = target_reference_id;
                anchor.ref.is_reverse = 1;
                const int32_t forward_query_pos = static_cast<int32_t>(query_pos_encoded >> 1);
                const int32_t reverse_query_pos = query_len
                    - (forward_query_pos + 1 - static_cast<int32_t>(query_span_value)) - 1;
                anchor.query.query_position = static_cast<uint32_t>(reverse_query_pos);
                anchor.query.query_span = query_span_value;
                anchor.query.flags = 0;
                anchor.query.segment_id = 0;
            }
            anchors.push_back(anchor);
        }
    }

    // Sort by reference position (ref_bits field), same as radix_sort_128x
    std::sort(anchors.begin(), anchors.end(),
              [](const Anchor& left, const Anchor& right) {
                  return left.ref_bits < right.ref_bits
                      || (left.ref_bits == right.ref_bits && left.query_bits < right.query_bits);
              });

    JustLog("[minimap_port] Collected {} seed anchors from {} query minimizers",
            anchors.size(), query_minimizers.size());
    return anchors;
}

// ============================================================================
// Phase 3: Chaining — now delegated to chaining modules
// ============================================================================
// compute_chaining_score is imported from bioseq_sequences.chaining.utils
// chain_anchors_scalar is imported from bioseq_sequences.chaining.scalar
// chain_anchors_simd is imported from bioseq_sequences.chaining.simd (in bioseq::simd::HWY_NAMESPACE)
// chain_anchors_rmq is imported from bioseq_sequences.chaining.rmq

// ============================================================================
// Phase 4: Convert chains to alignment regions (mm_gen_regs equivalent)
// ============================================================================

/**
 * @brief Compute seeded match length and block length for a region.
 *
 * Equivalent to mm_cal_fuzzy_len() in hit.c.
 *
 * match_length: approximate number of matching bases in seeded regions
 * block_length: approximate total aligned length in seeded regions
 */
void compute_fuzzy_lengths(AlignRegion& region, const std::vector<Anchor>& anchors) {
    region.match_length = 0;
    region.block_length = 0;
    if (region.seed_count <= 0) {
        return;
    }

    const int32_t anchor_begin = region.anchor_start;
    region.match_length = static_cast<int32_t>(anchors[anchor_begin].query.query_span);
    region.block_length = region.match_length;

    for (int32_t anchor_idx = anchor_begin + 1;
         anchor_idx < anchor_begin + region.seed_count;
         ++anchor_idx)
    {
        const int32_t span = static_cast<int32_t>(anchors[anchor_idx].query.query_span);
        const int32_t target_gap = static_cast<int32_t>(anchors[anchor_idx].ref.ref_position)
                                 - static_cast<int32_t>(anchors[anchor_idx - 1].ref.ref_position);
        const int32_t query_gap = static_cast<int32_t>(anchors[anchor_idx].query.query_position)
                                - static_cast<int32_t>(anchors[anchor_idx - 1].query.query_position);
        const int32_t max_gap = (target_gap > query_gap) ? target_gap : query_gap;
        region.block_length += max_gap;
        const bool both_gaps_exceed_span = (target_gap > span && query_gap > span);
        const int32_t min_gap = (target_gap < query_gap) ? target_gap : query_gap;
        const int32_t match_increment = both_gaps_exceed_span ? span : min_gap;
        region.match_length += match_increment;
    }
}

/**
 * @brief Set alignment coordinates from seed positions.
 *
 * Equivalent to mm_reg_set_coor() in hit.c.
 * Derives query_start, query_end, ref_start, ref_end from first and last anchor.
 */
void set_region_coordinates(
    AlignRegion& region,
    const std::vector<Anchor>& anchors,
    int32_t query_len)
{
    if (region.seed_count <= 0) {
        return;
    }
    const int32_t anchor_begin = region.anchor_start;
    const Anchor& first_anchor = anchors[anchor_begin];
    const Anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

    region.is_reverse = first_anchor.ref.is_reverse != 0;
    region.reference_id = static_cast<int32_t>(first_anchor.ref.reference_id);

    // Reference coordinates
    const int32_t first_span = static_cast<int32_t>(first_anchor.query.query_span);
    region.ref_start = static_cast<int32_t>(first_anchor.ref.ref_position) + 1 - first_span;
    region.ref_end = static_cast<int32_t>(last_anchor.ref.ref_position) + 1;
    if (region.ref_start < 0) {
        region.ref_start = 0;
    }

    // Query coordinates
    int32_t raw_query_start = static_cast<int32_t>(first_anchor.query.query_position)
                            + 1 - first_span;
    const int32_t raw_query_end = static_cast<int32_t>(last_anchor.query.query_position) + 1;
    if (raw_query_start < 0) {
        raw_query_start = 0;
    }

    if (!region.is_reverse) {
        region.query_start = raw_query_start;
        region.query_end = raw_query_end;
    } else {
        region.query_start = query_len - raw_query_end;
        region.query_end = query_len - raw_query_start;
    }

    compute_fuzzy_lengths(region, anchors);
}

/**
 * @brief Convert chains to alignment regions.
 *
 * Equivalent to mm_gen_regs() in hit.c:54-90.
 *
 * Sorts chains by score (descending) and creates AlignRegion records
 * with coordinates derived from the anchor positions.
 */
[[nodiscard]] std::vector<AlignRegion> generate_regions(
    std::vector<Chain>& chains,
    const std::vector<Anchor>& anchors,
    int32_t query_len)
{
    if (chains.empty()) {
        return {};
    }

    // Sort chains by score descending
    std::sort(chains.begin(), chains.end(),
              [](const Chain& left, const Chain& right) { return left.score > right.score; });

    std::vector<AlignRegion> regions(chains.size());
    for (size_t region_idx = 0; region_idx < chains.size(); ++region_idx) {
        regions[region_idx] = AlignRegion{
            .region_id      = static_cast<int32_t>(region_idx),
            .parent         = -1,
            .score          = chains[region_idx].score,
            .original_score = chains[region_idx].score,
            .seed_count     = chains[region_idx].anchor_count,
            .anchor_start   = chains[region_idx].anchor_start,
            .divergence     = -1.0f,
        };
        set_region_coordinates(regions[region_idx], anchors, query_len);
    }

    JustLog("[minimap_port] Generated {} alignment regions", regions.size());
    return regions;
}

// ============================================================================
// Phase 5: Primary/secondary classification (mm_set_parent equivalent)
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
void classify_primary_secondary(
    std::vector<AlignRegion>& regions,
    float mask_level,
    int32_t mask_len,
    int32_t sub_diff)
{
    const size_t region_count = regions.size();
    if (region_count == 0) {
        return;
    }

    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    // primary_indices holds indices of primary alignments
    std::vector<size_t> primary_indices;
    primary_indices.push_back(0);
    regions[0].parent = 0;

    std::vector<uint64_t> coverage_intervals;

    for (size_t region_idx = 1; region_idx < region_count; ++region_idx) {
        AlignRegion& current_region = regions[region_idx];
        const int32_t current_query_start = current_region.query_start;
        const int32_t current_query_end = current_region.query_end;
        int32_t uncovered_length = 0;

        // Compute coverage from existing primaries
        coverage_intervals.clear();
        for (const size_t primary_idx : primary_indices) {
            const AlignRegion& primary_region = regions[primary_idx];
            int32_t primary_query_start = primary_region.query_start;
            int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start
                || primary_query_start >= current_query_end)
            {
                continue;
            }
            primary_query_start = std::max(primary_query_start, current_query_start);
            primary_query_end = std::min(primary_query_end, current_query_end);
            coverage_intervals.push_back(
                (static_cast<uint64_t>(primary_query_start) << 32)
                | static_cast<uint32_t>(primary_query_end));
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
            AlignRegion& primary_region = regions[primary_idx];
            const int32_t primary_query_start = primary_region.query_start;
            const int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start
                || primary_query_start >= current_query_end)
            {
                continue;
            }

            const int32_t min_span = std::min(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t max_span = std::max(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t overlap_length = std::max(0,
                std::min(current_query_end, primary_query_end)
                - std::max(current_query_start, primary_query_start));

            if (min_span > 0 && max_span > 0 &&
                (static_cast<float>(overlap_length) / min_span -
                 static_cast<float>(uncovered_length) / max_span) > mask_level &&
                uncovered_length <= mask_len)
            {
                const int32_t resolved_parent = (primary_region.parent >= 0)
                    ? primary_region.parent : primary_region.region_id;
                current_region.parent = resolved_parent;
                primary_region.secondary_score = std::max(
                    primary_region.secondary_score, current_region.score);
                // Update secondary_dp_max for MAPQ (equivalent to hit.c:176)
                if (current_region.has_alignment && primary_region.has_alignment &&
                    (primary_region.reference_id != current_region.reference_id
                     || primary_region.ref_start != current_region.ref_start
                     || primary_region.ref_end != current_region.ref_end
                     || overlap_length != min_span))
                {
                    primary_region.secondary_dp_max = std::max(
                        primary_region.secondary_dp_max, current_region.dp_max_score);
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

    JustLog("[minimap_port] Classification: {} primary, {} secondary",
            primary_indices.size(), region_count - primary_indices.size());
}

// ============================================================================
// Phase 6: Filter secondary mappings (mm_select_sub equivalent)
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
void filter_secondary_regions(
    std::vector<AlignRegion>& regions,
    float pri_ratio,
    int32_t min_diff,
    int32_t best_n)
{
    if (regions.empty() || pri_ratio <= 0.0f) {
        return;
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
                 region.score + min_diff >= parent_region.score)
                && static_cast<int32_t>(secondary_kept_count) < best_n)
            {
                // Not identical coordinates
                const bool is_duplicate =
                    region.query_start == parent_region.query_start
                    && region.query_end == parent_region.query_end
                    && region.reference_id == parent_region.reference_id
                    && region.ref_start == parent_region.ref_start
                    && region.ref_end == parent_region.ref_end;
                if (!is_duplicate) {
                    keep[region_idx] = true;
                    ++secondary_kept_count;
                }
            }
        }
    }

    // Compact: move kept regions to front
    size_t write_pos = 0;
    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        if (keep[region_idx]) {
            if (write_pos < region_idx) {
                regions[write_pos] = std::move(regions[region_idx]);
            }
            ++write_pos;
        }
    }
    regions.resize(write_pos);

    // Re-index
    for (size_t region_idx = 0; region_idx < regions.size(); ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    JustLog("[minimap_port] After secondary filtering: {} regions remain", regions.size());
}

// ============================================================================
// Phase 7: Base-level alignment — dual-affine gap DP (KSW2 equivalent)
// ============================================================================

// generate_score_matrix is now ScoringMatrix<R>::generate in alignment_hwy_utils.

// ============================================================================
// KSW2 alignment wrapper
// ============================================================================

/**
 * @brief Dispatch to KSW2's dual-affine or affine gap alignment.
 *
 * Equivalent to mm_align_pair() in align.c:340-372.
 * If q == q2 && e == e2, uses affine gap (z2); otherwise dual-affine (d2).
 *
 * Accepts compact_sequence_range directly with sub-region indices so that
 * sequence encoding happens only inside the KSW2 core kernel.
 *
 * @tparam is_reversed  True for reverse-complement query strand
 * @tparam QueryRange   compact_sequence_range type for query
 * @tparam TargetRange  compact_sequence_range type for target
 * @tparam AlphabetDim  Scoring matrix dimension (default: alphabet_size + 1)
 */
template<
    bool is_reversed,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange>
[[nodiscard]] KswAlignResult ksw2_align_pair(
    const compact_sequence_range<QueryRange>& query_seq,
    const compact_sequence_range<TargetRange>& target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params,
    int align_bandwidth, int end_bonus, int zdrop, KswFlags ksw_flags)
{
    if (params.gap_open1 == params.gap_open2 && params.gap_ext1 == params.gap_ext2) {
        const KswZ2Params z2_params{
            .gap_open   = static_cast<int8_t>(params.gap_open1),
            .gap_extend = static_cast<int8_t>(params.gap_ext1),
            .band_width = align_bandwidth,
            .z_drop     = zdrop,
            .end_bonus  = end_bonus,
            .flags      = ksw_flags,
        };
        return HwyKsw2Aligner::z2_align<is_reversed>(
            query_seq, target_seq,
            query_start, query_end, target_start, target_end,
            scoring_matrix, z2_params);
    }
    const KswD2Params d2_params{
        .gap_open_short   = static_cast<int8_t>(params.gap_open1),
        .gap_extend_short = static_cast<int8_t>(params.gap_ext1),
        .gap_open_long    = static_cast<int8_t>(params.gap_open2),
        .gap_extend_long  = static_cast<int8_t>(params.gap_ext2),
        .band_width       = align_bandwidth,
        .z_drop           = zdrop,
        .end_bonus        = end_bonus,
        .flags            = ksw_flags,
    };
    return HwyKsw2Aligner::d2_align<is_reversed>(
        query_seq, target_seq,
        query_start, query_end, target_start, target_end,
        scoring_matrix, d2_params);
}

/**
 * @brief Dispatch to block aligner for single-affine gap alignment.
 *
 * Uses the first gap penalty pair (gap_open1, gap_ext1) only.
 * Block aligner does not support dual-affine gaps.
 */
template<
    bool is_reversed,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange>
[[nodiscard]] KswAlignResult block_aligner_align_pair(
    const compact_sequence_range<QueryRange>& query_seq,
    const compact_sequence_range<TargetRange>& target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params,
    [[maybe_unused]] int align_bandwidth,
    [[maybe_unused]] int end_bonus,
    int zdrop,
    KswFlags ksw_flags)
{
    BlockAlignerParams ba_params{
        .gap_open   = static_cast<int8_t>(params.gap_open1),
        .gap_extend = static_cast<int8_t>(params.gap_ext1),
        .min_size   = params.block_min_size,
        .max_size   = params.block_max_size,
        .x_drop     = ksw_flags.extz_only ? zdrop : -1,
        .flags      = ksw_flags,
    };
    return HwyBlockAligner::align<is_reversed>(
        query_seq, target_seq,
        query_start, query_end, target_start, target_end,
        scoring_matrix, ba_params);
}

// append_cigar is now in bioseq_cigar module.

/**
 * @brief Perform base-level alignment for each region using KSW2.
 *
 * Equivalent to align_regs() -> mm_align_skeleton() -> mm_align1() in align.c.
 *
 * For each region, follows the minimap2 3-part alignment strategy:
 *   1. Left extension: reversed query/target from first seed to extended boundary
 *   2. Gap filling: align the entire region between first and last seeds
 *   3. Right extension: query/target from last seed to extended boundary
 *
 * Sequences are passed as compact_sequence_range references with sub-region
 * indices — no pre-encoding into uint8_t vectors is needed. The KSW2 core
 * kernel handles encoding of the specific regions it needs.
 *
 * @tparam QueryRange   compact_sequence_range type for query
 * @tparam TargetRange  compact_sequence_range type for target
 * @tparam AlphabetDim  Scoring matrix dimension (default: alphabet_size + 1)
 */
template<
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange>
void align_regions(
    std::vector<AlignRegion>& regions,
    const std::vector<Anchor>& anchors,
    const compact_sequence_range<QueryRange>& query_seq,
    const compact_sequence_range<TargetRange>& target_seq,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params)
{
    const int32_t query_length = static_cast<int32_t>(query_seq.bp_size());
    const int32_t target_length = static_cast<int32_t>(target_seq.bp_size());
    const int32_t align_bandwidth = static_cast<int32_t>(params.bandwidth * 1.5 + 1.0);

    for (auto& region : regions) {
        if (region.seed_count <= 0) {
            continue;
        }

        const int32_t anchor_begin = region.anchor_start;
        const Anchor& first_anchor = anchors[anchor_begin];
        const Anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

        // Inner seed boundaries (where seeds are)
        const int32_t first_span = static_cast<int32_t>(first_anchor.query.query_span);
        int32_t seed_ref_start = static_cast<int32_t>(first_anchor.ref.ref_position)
                               + 1 - first_span;
        int32_t seed_query_start = static_cast<int32_t>(first_anchor.query.query_position)
                                 + 1 - first_span;
        const int32_t seed_ref_end = static_cast<int32_t>(last_anchor.ref.ref_position) + 1;
        const int32_t seed_query_end = static_cast<int32_t>(last_anchor.query.query_position) + 1;
        if (seed_ref_start < 0) { seed_ref_start = 0; }
        if (seed_query_start < 0) { seed_query_start = 0; }

        // Outer boundaries: extend beyond seeds (align.c:708-762 simplified)
        int32_t outer_ref_start = seed_ref_start;
        int32_t outer_query_start = seed_query_start;
        int32_t outer_ref_end = seed_ref_end;
        int32_t outer_query_end = seed_query_end;

        if (seed_query_start > 0 && seed_ref_start > 0) {
            int32_t extension_len = std::min(seed_query_start, params.max_gap);
            outer_query_start = seed_query_start - extension_len;
            const int32_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min(extension_len, params.max_gap);
            extension_len = std::min(extension_len, seed_ref_start);
            outer_ref_start = seed_ref_start - extension_len;
        }
        if (seed_query_end < query_length && seed_ref_end < target_length) {
            int32_t extension_len = std::min(query_length - seed_query_end, params.max_gap);
            outer_query_end = seed_query_end + extension_len;
            const int32_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min(extension_len, params.max_gap);
            extension_len = std::min(extension_len, target_length - seed_ref_end);
            outer_ref_end = seed_ref_end + extension_len;
        }
        if (outer_query_end > query_length) {
            outer_query_end = query_length;
        }

        region.cigar.clear();
        region.dp_score = 0;
        bool prior_phase_zdropped = false;

        JustLog("[minimap_port] Align regions. Determine actions.");

        // Dispatch the three alignment phases based on strand orientation.
        // The is_reversed template parameter selects reverse-complement query
        // encoding; left-extension reversal is handled inside ksw2_core_align
        // via the reverse_cigar flag.
        auto do_align = [&]<bool strand_reversed>() {
            // === 1. Left extension ===
            int32_t final_ref_start = seed_ref_start;
            int32_t final_query_start = seed_query_start;
            if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
                const KswAlignResult left_result = ksw2_align_pair<strand_reversed>(
                    query_seq, target_seq,
                    static_cast<size_t>(outer_query_start),
                    static_cast<size_t>(seed_query_start),
                    static_cast<size_t>(outer_ref_start),
                    static_cast<size_t>(seed_ref_start),
                    scoring_matrix, params,
                    align_bandwidth, params.end_bonus, params.zdrop,
                    KswFlags{.right_align = true, .extz_only = true, .reverse_cigar = true});
                if (!left_result.cigar.empty()) {
                    append_cigar(region.cigar, left_result.cigar);
                    region.dp_score += left_result.max;
                }
                const int32_t left_ref_offset = left_result.reach_end
                    ? left_result.mqe_t + 1 : left_result.max_t + 1;
                final_ref_start = seed_ref_start - left_ref_offset;
                const int32_t left_query_offset = left_result.reach_end
                    ? seed_query_start - outer_query_start : left_result.max_q + 1;
                final_query_start = seed_query_start - left_query_offset;
                prior_phase_zdropped = left_result.zdropped;
            } else {
                JustLog("[minimap_port] Skip left extension: seed_query_start={} > outer_query_start={} && seed_ref_start={} > outer_ref_start={}", seed_query_start, outer_query_start, seed_ref_start, outer_ref_start);
            }

            // === 2. Gap filling (entire region between first and last seed) ===
            int32_t final_ref_end = seed_ref_start;
            int32_t final_query_end = seed_query_start;
            if (region.seed_count > 1) {
                const int32_t gap_query_len = seed_query_end - seed_query_start;
                const int32_t gap_target_len = seed_ref_end - seed_ref_start;
                if (gap_query_len > 0 && gap_target_len > 0) {
                    const KswAlignResult fill_result = params.use_block_aligner
                        ? block_aligner_align_pair<strand_reversed>(
                            query_seq, target_seq,
                            static_cast<size_t>(seed_query_start),
                            static_cast<size_t>(seed_query_end),
                            static_cast<size_t>(seed_ref_start),
                            static_cast<size_t>(seed_ref_end),
                            scoring_matrix, params,
                            align_bandwidth, -1, params.zdrop,
                            KswFlags{.approx_max = true})
                        : ksw2_align_pair<strand_reversed>(
                            query_seq, target_seq,
                            static_cast<size_t>(seed_query_start),
                            static_cast<size_t>(seed_query_end),
                            static_cast<size_t>(seed_ref_start),
                            static_cast<size_t>(seed_ref_end),
                            scoring_matrix, params,
                            align_bandwidth, -1, params.zdrop,
                            KswFlags{.approx_max = true});
                    if (!fill_result.cigar.empty()) {
                        append_cigar(region.cigar, fill_result.cigar);
                    }
                    if (fill_result.zdropped) {
                        final_ref_end = seed_ref_start + (fill_result.max_t + 1);
                        final_query_end = seed_query_start + (fill_result.max_q + 1);
                        region.dp_score += fill_result.max;
                    } else {
                        final_ref_end = seed_ref_end;
                        final_query_end = seed_query_end;
                        region.dp_score += fill_result.score;
                    }
                    prior_phase_zdropped = fill_result.zdropped;
                }
            } else {
                JustLog("[minimap_port] Skip gap filling: region.seed_count={} > 1", region.seed_count);
                final_ref_end = seed_ref_end;
                final_query_end = seed_query_end;
            }

            // === 3. Right extension ===
            if (!prior_phase_zdropped
                && seed_query_end < outer_query_end
                && seed_ref_end < outer_ref_end)
            {
                const KswAlignResult right_result = ksw2_align_pair<strand_reversed>(
                    query_seq, target_seq,
                    static_cast<size_t>(seed_query_end),
                    static_cast<size_t>(outer_query_end),
                    static_cast<size_t>(seed_ref_end),
                    static_cast<size_t>(outer_ref_end),
                    scoring_matrix, params,
                    align_bandwidth, params.end_bonus, params.zdrop,
                    KswFlags{.extz_only = true});
                if (!right_result.cigar.empty()) {
                    append_cigar(region.cigar, right_result.cigar);
                    region.dp_score += right_result.max;
                }
                const int32_t right_ref_offset = right_result.reach_end
                    ? right_result.mqe_t + 1 : right_result.max_t + 1;
                final_ref_end = seed_ref_end + right_ref_offset;
                const int32_t right_query_offset = right_result.reach_end
                    ? outer_query_end - seed_query_end : right_result.max_q + 1;
                final_query_end = seed_query_end + right_query_offset;
            } else {
                JustLog("[minimap_port] Skip right extension: !prior_phase_zdropped={} && seed_query_end={} < outer_query_end={} && seed_ref_end={} < outer_ref_end={}", !prior_phase_zdropped, seed_query_end, outer_query_end, seed_ref_end, outer_ref_end);
            }

            // Set final coordinates from alignment extent (align.c:897-899)
            region.ref_start = final_ref_start;
            region.ref_end = final_ref_end;
            if (!region.is_reverse) {
                region.query_start = final_query_start;
                region.query_end = final_query_end;
            } else {
                region.query_start = query_length - final_query_end;
                region.query_end = query_length - final_query_start;
            }
        };

        if (region.is_reverse) {
            do_align.template operator()<true>();
        } else {
            do_align.template operator()<false>();
        }

        // Compute dp_max_score from CIGAR (simplified mm_update_extra)
        region.dp_max_score = region.dp_score;
        region.has_alignment = true;

        // Compute match_length/block_length from CIGAR
        int32_t cigar_match_count = 0;
        int32_t cigar_block_count = 0;
        for (const auto& cigar_op : region.cigar) {
            const int32_t op_len = static_cast<int32_t>(cigar_op.length);
            if (cigar_op.opcode == CIGAR_MATCH || cigar_op.opcode == CIGAR_EQ) {
                cigar_match_count += op_len;
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == CIGAR_X) {
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == CIGAR_INS || cigar_op.opcode == CIGAR_DEL) {
                cigar_block_count += op_len;
            }
        }
        if (cigar_match_count > 0) {
            region.match_length = cigar_match_count;
            const int32_t effective_block = (cigar_block_count > 0)
                ? cigar_block_count : region.block_length;
            region.block_length = effective_block;
        }
    }

    JustLog("[minimap_port] Aligned {} regions", regions.size());
}

// ============================================================================
// Phase 8: Compute mapping quality (mm_set_mapq equivalent)
// ============================================================================

// ============================================================================
// Phase 7b: Estimate divergence from seeds (mm_est_err equivalent)
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
void estimate_divergence_from_seeds(
    std::vector<AlignRegion>& regions,
    const std::vector<MinimapSketchData>& query_minimizers,
    const std::vector<Anchor>& anchors,
    int32_t query_len,
    int32_t kmer_size,
    int32_t target_len)
{
    if (query_minimizers.empty()) {
        return;
    }

    // Build sorted mini_pos array: stores last_pos values directly.
    // seed.c:127: mini_pos = q_span<<32 | q_pos>>1, where q_pos>>1 == last_pos.
    // esterr.c get_for_qpos on an anchor also resolves back to last_pos
    // for both strands (the reverse conversion cancels out), so mini_pos
    // simply contains the raw last_pos values sorted.
    std::vector<int32_t> minimizer_positions;
    minimizer_positions.reserve(query_minimizers.size());
    float span_sum = 0.0f;
    for (const auto& query_min : query_minimizers) {
        minimizer_positions.push_back(static_cast<int32_t>(query_min.last_pos));
        span_sum += static_cast<float>(query_min.span);
    }
    std::sort(minimizer_positions.begin(), minimizer_positions.end());
    const float avg_kmer_span = span_sum / static_cast<float>(query_minimizers.size());

    for (auto& region : regions) {
        region.divergence = -1.0f;
        if (region.seed_count <= 0) {
            continue;
        }

        const int32_t anchor_begin = region.anchor_start;

        // Get forward-strand position of the first anchor in the chain.
        // esterr.c:48: uses r->rev? last anchor : first anchor.
        // get_for_qpos resolves to last_pos for both strands.
        const Anchor& reference_anchor = region.is_reverse
            ? anchors[anchor_begin + region.seed_count - 1]
            : anchors[anchor_begin];

        // Compute forward-strand position without nested block scope
        int32_t first_forward_pos = static_cast<int32_t>(reference_anchor.query.query_position);
        if (reference_anchor.ref.is_reverse) {
            const int32_t anchor_span = static_cast<int32_t>(reference_anchor.query.query_span);
            first_forward_pos = query_len - 1 - (first_forward_pos + 1 - anchor_span);
        }

        // Binary search for first_forward_pos in minimizer_positions
        auto search_iter = std::lower_bound(
            minimizer_positions.begin(), minimizer_positions.end(), first_forward_pos);
        if (search_iter == minimizer_positions.end() || *search_iter != first_forward_pos) {
            continue; // Position not found — skip
        }
        const int32_t match_start_idx = static_cast<int32_t>(
            search_iter - minimizer_positions.begin());
        int32_t match_end_idx = match_start_idx;

        // Scan forward matching chain seeds (esterr.c:55-60)
        int32_t seed_match_count = 1;
        const int32_t minimizer_count = static_cast<int32_t>(minimizer_positions.size());
        for (int32_t seed_idx = 1, minimizer_idx = match_start_idx + 1;
             minimizer_idx < minimizer_count && seed_idx < region.seed_count;
             ++minimizer_idx)
        {
            const Anchor& chain_anchor = region.is_reverse
                ? anchors[anchor_begin + region.seed_count - 1 - seed_idx]
                : anchors[anchor_begin + seed_idx];

            // Compute forward-strand position for this anchor
            int32_t forward_pos = static_cast<int32_t>(chain_anchor.query.query_position);
            if (chain_anchor.ref.is_reverse) {
                const int32_t anchor_span = static_cast<int32_t>(chain_anchor.query.query_span);
                forward_pos = query_len - 1 - (forward_pos + 1 - anchor_span);
            }

            if (forward_pos == minimizer_positions[minimizer_idx]) {
                ++seed_idx;
                match_end_idx = minimizer_idx;
                ++seed_match_count;
            }
        }

        // n_tot = span of minimizer_positions between first and last match (esterr.c:61)
        int32_t total_minimizer_span = match_end_idx - match_start_idx + 1;

        // Edge corrections (esterr.c:62-63)
        if (region.query_start > avg_kmer_span && region.ref_start > avg_kmer_span) {
            ++total_minimizer_span;
        }
        if (query_len - region.query_start > avg_kmer_span
            && target_len - region.ref_end > avg_kmer_span)
        {
            ++total_minimizer_span;
        }

        // Divergence formula (esterr.c:64)
        if (seed_match_count >= total_minimizer_span) {
            region.divergence = 0.0f;
        } else {
            const double match_ratio = static_cast<double>(seed_match_count)
                                     / total_minimizer_span;
            region.divergence = static_cast<float>(
                1.0 - std::pow(match_ratio, 1.0 / avg_kmer_span));
        }
    }
}

// ============================================================================
// Phase 8: Compute mapping quality (mm_set_mapq equivalent)

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
void compute_mapping_quality(
    std::vector<AlignRegion>& regions,
    const Minimap2Params& params,
    int32_t rep_len)
{
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
    const float uniqueness_ratio = static_cast<float>(primary_score_sum) /
                                   static_cast<float>(primary_score_sum + rep_len);

    for (auto& region : regions) {
        if (region.parent != region.region_id) {
            region.mapq = 0; // secondaries get 0
            continue;
        }

        // Primary mapping
        const float score_factor = (region.score > 100) ? 1.0f : 0.01f * region.score;
        const float score_penalty = score_factor * uniqueness_ratio;
        const float raw_seed_penalty = (region.seed_count > 10)
            ? 1.0f : 0.1f * region.seed_count;
        const float seed_count_penalty = std::min(score_penalty, raw_seed_penalty);

        const int32_t effective_secondary_score = std::max(
            region.secondary_score, min_chain_sc);

        int32_t quality_value;
        // hit.c:447-467: three branches based on DP availability
        if (region.has_alignment && region.secondary_dp_max > 0
            && region.dp_max_score > 0)
        {
            // Formula A: with DP scores (hit.c:448-457)
            const float identity = (region.block_length > 0)
                ? static_cast<float>(region.match_length) / region.block_length
                : 0.f;
            const float score_ratio =
                static_cast<float>(region.secondary_dp_max) * effective_secondary_score
                / (static_cast<float>(region.dp_max_score) * region.original_score);
            quality_value = static_cast<int32_t>(
                identity * seed_count_penalty * quality_coefficient
                * (1.0f - score_ratio * score_ratio)
                * std::log(static_cast<float>(region.dp_max_score) / match_sc));

            // BWA-MEM-like alternative (hit.c:455)
            const int32_t quality_alt = static_cast<int32_t>(
                6.02f * identity * identity
                * (region.dp_max_score - region.secondary_dp_max) / match_sc + 0.499f);
            quality_value = std::min(quality_value, quality_alt);
        } else {
            // No secondary_dp_max available: use chain score formula (hit.c:460-467)
            const float score_ratio = static_cast<float>(effective_secondary_score)
                                    / region.original_score;
            if (region.has_alignment) {
                const float identity = (region.block_length > 0)
                    ? static_cast<float>(region.match_length) / region.block_length
                    : 0.f;
                quality_value = static_cast<int32_t>(
                    identity * seed_count_penalty * quality_coefficient
                    * (1.0f - score_ratio)
                    * std::log(static_cast<float>(region.dp_max_score) / match_sc));
            } else {
                // No DP at all — use chain score (hit.c:466)
                quality_value = static_cast<int32_t>(
                    seed_count_penalty * quality_coefficient
                    * (1.0f - score_ratio)
                    * std::log(static_cast<float>(region.score)));
            }
        }

        // Subtract secondary_count penalty (hit.c:469)
        quality_value -= static_cast<int32_t>(
            4.343f * std::log(static_cast<float>(region.secondary_count + 1)) + 0.499f);
        quality_value = std::max(quality_value, 0);
        region.mapq = std::min(quality_value, 60);

        // Ensure non-zero if clearly best (hit.c:472)
        if (region.has_alignment && region.dp_max_score > region.secondary_dp_max
            && region.mapq == 0)
        {
            region.mapq = 1;
        }
    }

    JustLog("[minimap_port] Computed MAPQ for {} regions", regions.size());
}

// ============================================================================
// Phase 9: Convert to MatchResult (PAF output)
// ============================================================================

/**
 * @brief Convert internal AlignRegion to PAF-like MatchResult.
 *
 * Fields map to PAF columns as documented in mm_write_paf4() (format.c:427-460).
 */
[[nodiscard]] MatchResult region_to_match_result(
    AlignRegion&& region,
    int32_t query_len,
    int32_t target_len)
{
    return MatchResult{
        .query_len             = query_len,
        .query_start           = region.query_start,
        .query_end             = region.query_end,
        .is_reverse            = region.is_reverse,
        .target_len            = target_len,
        .target_start          = region.ref_start,
        .target_end            = region.ref_end,
        .num_matches           = region.match_length,
        .block_length          = region.block_length,
        .mapq                  = region.mapq,
        .chain_score           = region.score,
        .dp_score              = region.dp_score,
        .dp_max_score          = region.dp_max_score,
        .seed_count            = region.seed_count,
        .secondary_chain_score = region.secondary_score,
        .divergence            = region.divergence,
        .is_primary            = (region.parent == region.region_id),
        .has_cigar             = region.has_alignment,
        .cigar                 = std::move(region.cigar),
    };
}

// ============================================================================
// PAF output — MatchResultCollection
// ============================================================================

// CIGAR_OP_CHAR and format_cigar are now in bioseq_cigar module.

/**
 * @brief Owning container of alignment results with PAF output formatting.
 *
 * Wraps a std::vector<MatchResult> and provides a print() method that outputs
 * results in exact minimap2 PAF format, reproducing mm_write_paf4()
 * (format.c:427-460) with auxiliary tags from write_tags() (format.c:399-425).
 */
struct MatchResultCollection {
    std::vector<MatchResult> results;

    MatchResultCollection() = default;
    explicit MatchResultCollection(std::vector<MatchResult>&& results_vec)
        : results(std::move(results_vec)) {}

    [[nodiscard]] size_t size() const { return results.size(); }
    [[nodiscard]] bool empty() const { return results.empty(); }

    [[nodiscard]] std::span<const MatchResult> view() const {
        return std::span<const MatchResult>(results);
    }

    /**
     * @brief Print alignment results in exact minimap2 PAF format.
     *
     * PAF line format (tab-separated):
     *   col 1:  query name             -- user-provided
     *   col 2:  query sequence length
     *   col 3:  query start (0-based, closed)
     *   col 4:  query end   (0-based, open)
     *   col 5:  relative strand '+' or '-'
     *   col 6:  target name            -- user-provided
     *   col 7:  target sequence length
     *   col 8:  target start on original strand (0-based)
     *   col 9:  target end   on original strand (0-based)
     *   col 10: number of residue matches  (match_length)
     *   col 11: alignment block length     (block_length)
     *   col 12: mapping quality (0-255)
     *
     * Auxiliary tags: NM, ms, AS, nn, tp, cm, s1, s2, de/dv, rl, cg
     *
     * @param query_name  Query sequence name (col 1)
     * @param target_name Target sequence name (col 6)
     */
    void print(const std::string& query_name  = "query",
               const std::string& target_name = "target") const
    {
        if (results.empty()) {
            JustLog("{}\t0\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0", query_name);
            return;
        }

        for (const auto& match : results) {
            const char strand_char = match.is_reverse ? '-' : '+';
            const char type_char = match.is_primary ? 'P' : 'S';

            std::string line = fmt::format(
                "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                query_name, match.query_len, match.query_start, match.query_end,
                strand_char, target_name, match.target_len, match.target_start,
                match.target_end, match.num_matches, match.block_length, match.mapq);

            if (match.has_cigar && !match.cigar.empty()) {
                const int32_t edit_distance = std::max(0,
                    match.block_length - match.num_matches);
                line += fmt::format("\tNM:i:{}\tms:i:{}\tAS:i:{}\tnn:i:0",
                    edit_distance, match.dp_max_score, match.dp_score);
            }

            line += fmt::format("\ttp:A:{}", type_char);
            line += fmt::format("\tcm:i:{}", match.seed_count);
            line += fmt::format("\ts1:i:{}", match.chain_score);

            if (match.is_primary) {
                line += fmt::format("\ts2:i:{}", match.secondary_chain_score);
            }

            if (match.divergence >= 0.0f && match.divergence <= 1.0f) {
                const char* divergence_tag = (match.has_cigar && !match.cigar.empty())
                    ? "\tde:f:" : "\tdv:f:";
                if (match.divergence == 0.0f) {
                    line += divergence_tag;
                    line += '0';
                } else {
                    line += fmt::format("{}{:.4f}", divergence_tag,
                        static_cast<double>(match.divergence));
                }
            }

            line += "\trl:i:0";

            if (match.has_cigar && !match.cigar.empty()) {
                line += "\tcg:Z:";
                line += format_cigar(match.cigar);
            }

            JustLog("{}", line);
        }
    }
};

/**
 * @brief Print alignment results in exact minimap2 PAF format.
 *
 * Free function wrapper for backwards compatibility. Delegates to
 * MatchResultCollection::print().
 */
void print_match_results(
    const MatchResultCollection& collection,
    const std::string& query_name  = "query",
    const std::string& target_name = "target")
{
    collection.print(query_name, target_name);
}

// ============================================================================
// Main entry point
// ============================================================================

/**
 * @brief Run the complete minimap2 mapping pipeline.
 *
 * This is the main entry point that executes all phases of the minimap2
 * algorithm. It assumes minimizers have already been collected for both
 * query and target sequences.
 *
 * Pipeline steps:
 *   1. Build target index from target minimizers
 *   2. Collect seed hits (lookup query minimizers in target index)
 *   3. Chain seeds using DP (mg_lchain_dp)
 *   4. Generate alignment regions (mm_gen_regs)
 *   5. Classify primary/secondary (mm_set_parent)
 *   6. Filter secondaries (mm_select_sub)
 *   7. Base-level alignment via dual-affine DP (KSW2)
 *   8. Compute mapping quality (mm_set_mapq2)
 *   9. Convert to PAF-like MatchResult records
 *
 * @tparam QueryRange   compact_sequence_range type for query (e.g. sequence_custom_alphabet<seq::dna4, uint64_t>)
 * @tparam TargetRange  compact_sequence_range type for target
 * @param query_seq           Query sequence as compact_sequence_range
 * @param target_seq          Target sequence as compact_sequence_range
 * @param query_minimizers    Pre-computed minimizers for the query
 * @param target_minimizers   Pre-computed minimizers for the target
 * @param params              Algorithm parameters (defaults match minimap2)
 * @return MatchResultCollection containing PAF-like alignment results
 */
template<supports_compact_sequence_range QueryRange, supports_compact_sequence_range TargetRange>
[[nodiscard]] MatchResultCollection minimap2_port(
    const compact_sequence_range<QueryRange>& query_seq,
    const compact_sequence_range<TargetRange>& target_seq,
    const std::vector<MinimapSketchData>& query_minimizers,
    const std::vector<MinimapSketchData>& target_minimizers,
    const Minimap2Params& params = Minimap2Params{})
{
    using query_traits = typename compact_sequence_range<QueryRange>::traits;
    using target_traits = typename compact_sequence_range<TargetRange>::traits;
    static_assert(
        std::is_same_v<typename query_traits::alphabet_type, typename target_traits::alphabet_type>,
        "Query and target sequences must use the same alphabet type");

    constexpr size_t alphabet_size = seqan3::alphabet_size<typename query_traits::alphabet_type>;

    const int32_t query_len  = static_cast<int32_t>(query_seq.bp_size());
    const int32_t target_len = static_cast<int32_t>(target_seq.bp_size());

    JustLog("[minimap_port] Starting minimap2_port: query_len={}, target_len={}, "
            "query_minimizers={}, target_minimizers={}, alphabet_size={}",
            query_len, target_len, query_minimizers.size(), target_minimizers.size(),
            alphabet_size);

    if (query_len == 0 || target_len == 0) {
        JustLog("[minimap_port] Empty input — returning no results");
        return MatchResultCollection{};
    }

    // ---- Phase 1: Build target index ----
    TargetIndex target_index;
    target_index.build(target_minimizers, target_len);

    // ---- Phase 2: Collect seed hits ----
    auto anchors = collect_seed_hits(
        query_minimizers, target_index, query_len, params.max_occ);

    if (anchors.empty()) {
        JustLog("[minimap_port] No seed hits found — returning no results");
        return MatchResultCollection{};
    }

    // ---- Phase 3: Chain seeds ----
    // Build chaining params and dispatch to the selected algorithm
    const auto chaining_params = params.to_chaining_params();

    auto [chains, reordered_anchors] = [&]()
        -> std::pair<std::vector<Chain>, std::vector<Anchor>>
    {
        switch (chaining_params.chaining_mode) {
            case ChainingMode::SIMD_DP:
                return chain_anchors_simd(anchors, chaining_params);
            case ChainingMode::RMQ:
                return chain_anchors_rmq(anchors, chaining_params);
            case ChainingMode::SCALAR_DP:
            default:
                return chain_anchors_scalar(anchors, chaining_params);
        }
    }();

    if (chains.empty()) {
        JustLog("[minimap_port] No chains formed — returning no results");
        return MatchResultCollection{};
    }

    // ---- Phase 4: Generate alignment regions ----
    auto regions = generate_regions(chains, reordered_anchors, query_len);

    // ---- Phase 5 (chain_post): Primary/secondary classification + filter ----
    // Equivalent to chain_post() in map.c:559-566
    const int32_t sub_diff = params.match_score * 2 + params.mismatch_pen;
    classify_primary_secondary(regions, params.mask_level, params.mask_len, sub_diff);
    const int32_t min_diff = params.kmer_size * 2;
    filter_secondary_regions(regions, params.pri_ratio, min_diff, params.best_n);

    // ---- Phase 6: Estimate divergence from seeds (mm_est_err equivalent) ----
    // This is done BEFORE alignment (map.c:783) and provides dv:f tag
    estimate_divergence_from_seeds(regions, query_minimizers, reordered_anchors,
                                   query_len, params.kmer_size, target_len);

    // ---- Phase 7: Base-level alignment (only when CIGAR requested) ----
    // map.c:568-570: align_regs() returns immediately if !(opt->flag & MM_F_CIGAR)
    if (params.do_cigar) {
        // Build unified scoring matrix for KSW2 and block aligner.
        const auto scoring_matrix = ScoringMatrix<QueryRange>::generate(
            static_cast<int8_t>(params.match_score),
            static_cast<int8_t>(params.mismatch_pen),
            static_cast<int8_t>(params.sc_ambi));

        // Sequences are passed directly — encoding of specific sub-regions
        // happens lazily inside the KSW2 core kernel.
        align_regions(regions, reordered_anchors,
                      query_seq, target_seq,
                      scoring_matrix, params);

        // Re-classify after DP (like align_regs does in map.c:572-574)
        classify_primary_secondary(regions, params.mask_level, params.mask_len, sub_diff);
        filter_secondary_regions(regions, params.pri_ratio, min_diff, params.best_n);
    }

    // ---- Phase 8: Compute MAPQ ----
    compute_mapping_quality(regions, params, 0);

    // ---- Phase 9: Convert to MatchResult ----
    std::vector<MatchResult> results;
    results.reserve(regions.size());
    for (auto& region : regions) {
        results.push_back(region_to_match_result(std::move(region), query_len, target_len));
    }

    JustLog("[minimap_port] Pipeline complete: {} alignment results", results.size());
    return MatchResultCollection(std::move(results));
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

} // export
