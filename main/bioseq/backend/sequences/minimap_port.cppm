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
import alignment;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

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
    /// Equivalent to MM_F_CIGAR flag in minimap2. Default false matches the
    /// test invocation without -a/-c flags.
    bool    do_cigar       = false;
};

// ============================================================================
// CIGAR operation codes — same as minimap2 (minimap.h)
// ============================================================================

constexpr uint32_t CIGAR_MATCH     = 0;
constexpr uint32_t CIGAR_INS       = 1;
constexpr uint32_t CIGAR_DEL       = 2;
constexpr uint32_t CIGAR_EQ        = 7;
constexpr uint32_t CIGAR_X         = 8;

// ============================================================================
// Result structures
// ============================================================================

/**
 * @brief Single CIGAR operation: length + operation code.
 */
struct CigarOp {
    uint32_t length;
    uint32_t opcode;
};

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

/**
 * @brief Bitfield data for anchor reference (x) component.
 *
 * Layout mirrors minimap2 mm128_t.x conventions:
 *   x: is_reverse<<63 | reference_id<<32 | ref_position
 *
 * For our single-target case, reference_id is always 0.
 */
struct AnchorRefFields {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint64_t ref_position: 32;    ///< lower 32 bits: reference position
    uint64_t reference_id: 31;    ///< bits 32-62: reference sequence ID
    uint64_t is_reverse: 1;       ///< bit 63: reverse strand flag
#elif __BYTE_ORDER == __BIG_ENDIAN
    uint64_t is_reverse: 1;
    uint64_t reference_id: 31;
    uint64_t ref_position: 32;
#endif
    int : 0;
};

/**
 * @brief Bitfield data for anchor query (y) component.
 *
 * Layout mirrors minimap2 mm128_t.y conventions:
 *   y: segment_id<<48 | flags<<40 | query_span<<32 | query_position
 */
struct AnchorQueryFields {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint64_t query_position: 32;  ///< lower 32 bits: query position
    uint64_t query_span: 8;       ///< bits 32-39: query k-mer span
    uint64_t flags: 8;            ///< bits 40-47: flags
    uint64_t segment_id: 16;      ///< bits 48-63: segment ID
#elif __BYTE_ORDER == __BIG_ENDIAN
    uint64_t segment_id: 16;
    uint64_t flags: 8;
    uint64_t query_span: 8;
    uint64_t query_position: 32;
#endif
    int : 0;
};

/**
 * @brief Anchor for chaining. Equivalent to mm128_t in collect_seed_hits output.
 *
 * Uses unions to provide both clean bitfield access (ref/query structs) and
 * raw uint64_t access (ref_bits/query_bits) for sorting and arithmetic.
 *
 * Bitfield access examples:
 *   anchor.ref.ref_position   (was: static_cast<int32_t>(x))
 *   anchor.ref.reference_id   (was: (x << 1) >> 33)
 *   anchor.ref.is_reverse     (was: x >> 63)
 *   anchor.query.query_span   (was: (y >> 32) & 0xff)
 *   anchor.query.query_position (was: static_cast<int32_t>(y))
 */
struct Anchor {
    union {
        AnchorRefFields ref;
        uint64_t ref_bits = 0;    ///< Raw value for sorting/arithmetic on reference data
    };
    union {
        AnchorQueryFields query;
        uint64_t query_bits = 0;  ///< Raw value for sorting/arithmetic on query data
    };
};

/**
 * @brief One chain of anchors, with metadata.
 *
 * Equivalent to one entry in the u[] array after chaining.
 */
struct Chain {
    int32_t score        = 0;    ///< chain score (upper 32 bits of u[])
    int32_t anchor_count = 0;    ///< anchor count (lower 32 bits of u[])
    int32_t anchor_start = 0;    ///< start index in the reordered anchor array
};

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

// ============================================================================
// Fast approximate log2 — matches minimap2's mg_log2 (mmpriv.h:139-147)
// ============================================================================

/**
 * @brief Fast log2 approximation using IEEE 754 float bit tricks.
 *
 * Accurate to ~3 significant figures for x >= 2.
 * Matches minimap2 mg_log2() used in gap penalty computation.
 */
[[nodiscard]] inline float fast_log2(float input) {
    uint32_t float_bits = std::bit_cast<uint32_t>(input);
    float log_result = static_cast<float>(((float_bits >> 23) & 255) - 128);
    float_bits &= ~(255u << 23);
    float_bits += 127u << 23;
    const float mantissa_approx = std::bit_cast<float>(float_bits);
    log_result += (-0.34484843f * mantissa_approx + 2.02466578f) * mantissa_approx - 0.67487759f;
    return log_result;
}

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
// Phase 3: Chaining — DP-based (mg_lchain_dp equivalent)
// ============================================================================

/**
 * @brief Compute chaining score between two anchors.
 *
 * Equivalent to comput_sc() in lchain.c:115-140.
 *
 * The score is: min(predecessor_span, min(ref_distance, query_distance)) minus gap penalties.
 * Gap penalty = linear_penalty + 0.5 * log_penalty, where:
 *   linear_penalty = chain_gap_penalty * |ref_distance - query_distance|
 *                  + chain_skip_penalty * min(ref_distance, query_distance)
 *   log_penalty = log2(|ref_distance - query_distance| + 1)
 *
 * Returns INT32_MIN if the pair is invalid (too far, wrong order, etc.)
 */
[[nodiscard]] inline int32_t compute_chaining_score(
    const Anchor& current_anchor, const Anchor& predecessor_anchor,
    int32_t max_dist_x, int32_t max_dist_y, int32_t chain_bandwidth,
    float chain_gap_penalty, float chain_skip_penalty)
{
    const int32_t query_distance = static_cast<int32_t>(current_anchor.query.query_position)
                                 - static_cast<int32_t>(predecessor_anchor.query.query_position);
    if (query_distance <= 0 || query_distance > max_dist_x) {
        return std::numeric_limits<int32_t>::min();
    }

    const int32_t ref_distance = static_cast<int32_t>(current_anchor.ref_bits - predecessor_anchor.ref_bits);
    if (ref_distance == 0 || query_distance > max_dist_y) {
        return std::numeric_limits<int32_t>::min();
    }

    const int32_t distance_difference = (ref_distance > query_distance)
        ? ref_distance - query_distance
        : query_distance - ref_distance;
    if (distance_difference > chain_bandwidth) {
        return std::numeric_limits<int32_t>::min();
    }

    const int32_t min_distance = (ref_distance < query_distance) ? ref_distance : query_distance;
    const int32_t predecessor_span = static_cast<int32_t>(predecessor_anchor.query.query_span);
    int32_t score = (predecessor_span < min_distance) ? predecessor_span : min_distance;

    if (distance_difference != 0 || min_distance > predecessor_span) {
        const float linear_penalty = chain_gap_penalty * static_cast<float>(distance_difference)
                                   + chain_skip_penalty * static_cast<float>(min_distance);
        const float log_penalty = (distance_difference >= 1)
            ? fast_log2(static_cast<float>(distance_difference + 1))
            : 0.0f;
        score -= static_cast<int32_t>(linear_penalty + 0.5f * log_penalty);
    }
    return score;
}

/**
 * @brief Chain anchors using dynamic programming.
 *
 * Equivalent to mg_lchain_dp() in lchain.c:150-219.
 *
 * Algorithm:
 *   For each anchor (sorted by ref position):
 *     1. Search backwards through prior anchors in [window_start, current-1]
 *     2. Compute chaining score: pair_score(current, candidate) + best_scores[candidate]
 *     3. Track best predecessor and score
 *     4. Apply max_skip early termination
 *     5. Try the globally best anchor (global_best_idx) as fallback
 *
 *   Backtrack from highest-scoring anchors to extract chains.
 *
 * @param anchors  Sorted anchor array (modified: reordered on output)
 * @param params   Minimap2 parameters
 * @return Vector of chains with scores and anchor ranges
 */
[[nodiscard]] std::pair<std::vector<Chain>, std::vector<Anchor>> chain_anchors_dp(
    std::vector<Anchor>& anchors,
    const Minimap2Params& params)
{
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());
    if (anchor_count == 0) {
        return {{}, {}};
    }

    // map.c:722-723: chain_gap_penalty = opt->chain_gap_scale * 0.01 * mi->k
    const float chain_gap_penalty = params.chain_gap_scale * 0.01f
                                  * static_cast<float>(params.kmer_size);
    const float chain_skip_penalty = params.chain_skip_scale * 0.01f
                                   * static_cast<float>(params.kmer_size);
    int32_t max_dist_x = params.max_gap;
    int32_t max_dist_y = params.max_gap;
    const int32_t chain_bandwidth = params.bandwidth;
    const int32_t max_skip = params.max_chain_skip;
    const int32_t max_iter = params.max_chain_iter;
    const int32_t min_seed_count = params.min_seed_count;
    const int32_t min_score = params.min_chain_score;
    const int32_t max_drop = chain_bandwidth;

    if (max_dist_x < chain_bandwidth) { max_dist_x = chain_bandwidth; }
    if (max_dist_y < chain_bandwidth) { max_dist_y = chain_bandwidth; }

    // DP arrays
    std::vector<int32_t> best_scores(anchor_count);       // best score ending at anchor
    std::vector<int64_t> predecessors(anchor_count, -1);   // predecessor index
    std::vector<int32_t> peak_scores(anchor_count);        // peak score seen along chain
    std::vector<int32_t> visited(anchor_count, 0);         // visited marker

    int64_t window_start = 0;
    int64_t global_best_idx = -1;

    // --- Forward DP pass ---
    // Equivalent to lchain.c lines 171-206
    for (int64_t current_idx = 0; current_idx < anchor_count; ++current_idx) {
        int64_t best_predecessor_idx = -1;
        int32_t best_score = static_cast<int32_t>(anchors[current_idx].query.query_span);
        int32_t skip_count = 0;

        // Advance window start: skip anchors on different chromosomes or too far
        while (window_start < current_idx &&
               (anchors[current_idx].ref_bits >> 32 != anchors[window_start].ref_bits >> 32 ||
                anchors[current_idx].ref_bits > anchors[window_start].ref_bits
                    + static_cast<uint64_t>(max_dist_x)))
        {
            ++window_start;
        }
        if (current_idx - window_start > max_iter) {
            window_start = current_idx - max_iter;
        }

        int64_t search_end = window_start;

        // Search backwards for best predecessor
        for (int64_t candidate_idx = current_idx - 1; candidate_idx >= window_start; --candidate_idx) {
            const int32_t pair_score = compute_chaining_score(
                anchors[current_idx], anchors[candidate_idx],
                max_dist_x, max_dist_y, chain_bandwidth,
                chain_gap_penalty, chain_skip_penalty);
            if (pair_score == std::numeric_limits<int32_t>::min()) {
                continue;
            }

            const int32_t total_score = pair_score + best_scores[candidate_idx];
            if (total_score > best_score) {
                best_score = total_score;
                best_predecessor_idx = candidate_idx;
                if (skip_count > 0) {
                    --skip_count;
                }
            } else if (visited[candidate_idx] == static_cast<int32_t>(current_idx)) {
                if (++skip_count > max_skip) {
                    search_end = candidate_idx;
                    break;
                }
            }
            if (predecessors[candidate_idx] >= 0) {
                visited[predecessors[candidate_idx]] = static_cast<int32_t>(current_idx);
            }
        }

        // Try the globally best anchor in the window (global_best_idx optimization)
        if (global_best_idx < 0 ||
            anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                > static_cast<uint64_t>(max_dist_x))
        {
            int32_t best_global = std::numeric_limits<int32_t>::min();
            global_best_idx = -1;
            for (int64_t scan_idx = current_idx - 1; scan_idx >= window_start; --scan_idx) {
                if (best_global < best_scores[scan_idx]) {
                    best_global = best_scores[scan_idx];
                    global_best_idx = scan_idx;
                }
            }
        }
        if (global_best_idx >= 0 && global_best_idx < search_end) {
            const int32_t global_pair_score = compute_chaining_score(
                anchors[current_idx], anchors[global_best_idx],
                max_dist_x, max_dist_y, chain_bandwidth,
                chain_gap_penalty, chain_skip_penalty);
            if (global_pair_score != std::numeric_limits<int32_t>::min()
                && best_score < global_pair_score + best_scores[global_best_idx])
            {
                best_score = global_pair_score + best_scores[global_best_idx];
                best_predecessor_idx = global_best_idx;
            }
        }

        best_scores[current_idx] = best_score;
        predecessors[current_idx] = best_predecessor_idx;
        const bool predecessor_has_higher_peak = best_predecessor_idx >= 0
            && peak_scores[best_predecessor_idx] > best_score;
        peak_scores[current_idx] = predecessor_has_higher_peak
            ? peak_scores[best_predecessor_idx]
            : best_score;

        if (global_best_idx < 0 ||
            (anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                <= static_cast<uint64_t>(max_dist_x)
             && best_scores[global_best_idx] < best_scores[current_idx]))
        {
            global_best_idx = current_idx;
        }
    }

    // --- Backtracking ---
    // Equivalent to mg_chain_backtrack() in lchain.c:29-78
    struct ScoredAnchorIndex {
        int32_t score;
        int64_t anchor_index;
    };
    std::vector<ScoredAnchorIndex> scored_indices;
    scored_indices.reserve(anchor_count);
    for (int64_t anchor_idx = 0; anchor_idx < anchor_count; ++anchor_idx) {
        if (best_scores[anchor_idx] >= min_score) {
            scored_indices.push_back({best_scores[anchor_idx], anchor_idx});
        }
    }

    std::sort(scored_indices.begin(), scored_indices.end(),
              [](const ScoredAnchorIndex& left, const ScoredAnchorIndex& right) {
                  return left.score < right.score;
              });

    std::fill(visited.begin(), visited.end(), 0);

    std::vector<Chain> chains;
    std::vector<Anchor> reordered_anchors;
    std::vector<int64_t> chain_anchor_indices;

    for (int64_t rank_idx = static_cast<int64_t>(scored_indices.size()) - 1; rank_idx >= 0; --rank_idx) {
        if (visited[scored_indices[rank_idx].anchor_index] != 0) {
            continue;
        }

        chain_anchor_indices.clear();
        int64_t trace_idx;
        for (trace_idx = scored_indices[rank_idx].anchor_index;
             trace_idx >= 0 && visited[trace_idx] == 0;
             trace_idx = predecessors[trace_idx])
        {
            chain_anchor_indices.push_back(trace_idx);
            visited[trace_idx] = 1;
        }

        const int32_t chain_score = (trace_idx < 0)
            ? scored_indices[rank_idx].score
            : scored_indices[rank_idx].score - best_scores[trace_idx];
        const int32_t chain_length = static_cast<int32_t>(chain_anchor_indices.size());

        if (chain_score >= min_score && chain_length >= min_seed_count) {
            // Reverse to forward order
            std::reverse(chain_anchor_indices.begin(), chain_anchor_indices.end());

            Chain chain;
            chain.score = chain_score;
            chain.anchor_count = chain_length;
            chain.anchor_start = static_cast<int32_t>(reordered_anchors.size());
            chains.push_back(chain);

            for (const int64_t anchor_idx : chain_anchor_indices) {
                reordered_anchors.push_back(anchors[anchor_idx]);
            }
        } else {
            // Unmark rejected chain
            for (const int64_t anchor_idx : chain_anchor_indices) {
                visited[anchor_idx] = 0;
            }
        }
    }

    JustLog("[minimap_port] Chaining: {} chains from {} anchors", chains.size(), anchor_count);
    return {std::move(chains), std::move(reordered_anchors)};
}

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

/**
 * @brief Generate the 5x5 scoring matrix for DNA alignment.
 *
 * Equivalent to ksw_gen_simple_mat() in align.c:15-27.
 * Matrix is 5x5 for {A, C, G, T, N} = {0, 1, 2, 3, 4}.
 * Diagonal = match_score, off-diagonal = -mismatch_pen.
 * Row/col 4 (N) scores -sc_ambi against everything.
 */
constexpr std::array<int8_t, 25> generate_score_matrix(
    int8_t match_score, int8_t mismatch_pen, int8_t sc_ambi)
{
    std::array<int8_t, 25> matrix{};
    const int8_t positive_match = match_score > 0
        ? match_score : static_cast<int8_t>(-match_score);
    const int8_t negative_mismatch = mismatch_pen > 0
        ? static_cast<int8_t>(-mismatch_pen) : mismatch_pen;
    const int8_t ambiguity_penalty = sc_ambi > 0
        ? static_cast<int8_t>(-sc_ambi) : sc_ambi;
    for (int32_t matrix_row = 0; matrix_row < 4; ++matrix_row) {
        for (int32_t matrix_col = 0; matrix_col < 4; ++matrix_col) {
            const int8_t cell_score = (matrix_row == matrix_col)
                ? positive_match : negative_mismatch;
            matrix[matrix_row * 5 + matrix_col] = cell_score;
        }
        matrix[matrix_row * 5 + 4] = ambiguity_penalty;
    }
    for (int32_t matrix_col = 0; matrix_col < 5; ++matrix_col) {
        matrix[4 * 5 + matrix_col] = ambiguity_penalty;
    }
    return matrix;
}

/**
 * @brief Convert a dna4 base to 4-bit encoding (0=A, 1=C, 2=G, 3=T).
 */
[[nodiscard]] inline uint8_t dna4_to_nt4(seq::dna4 base) {
    return static_cast<uint8_t>(base.to_rank());
}

/**
 * @brief Encode a dna4 sequence to uint8_t array.
 */
[[nodiscard]] std::vector<uint8_t> encode_sequence(const std::vector<seq::dna4>& sequence) {
    std::vector<uint8_t> encoded(sequence.size());
    for (size_t nucleotide_pos = 0; nucleotide_pos < sequence.size(); ++nucleotide_pos) {
        encoded[nucleotide_pos] = dna4_to_nt4(sequence[nucleotide_pos]);
    }
    return encoded;
}

/**
 * @brief Reverse complement an encoded sequence.
 */
[[nodiscard]] std::vector<uint8_t> reverse_complement(const std::vector<uint8_t>& sequence) {
    std::vector<uint8_t> rev_comp(sequence.size());
    for (size_t nucleotide_pos = 0; nucleotide_pos < sequence.size(); ++nucleotide_pos) {
        const uint8_t base = sequence[sequence.size() - 1 - nucleotide_pos];
        rev_comp[nucleotide_pos] = (base < 4)
            ? static_cast<uint8_t>(3 - base)
            : static_cast<uint8_t>(4);
    }
    return rev_comp;
}

// ============================================================================
// KSW2 alignment wrapper — calls Aligner<ALIGN_ALGORITHM_KSW2>::d2()/z2()
// ============================================================================

/**
 * @brief Dispatch to KSW2's dual-affine or affine gap alignment.
 *
 * Equivalent to mm_align_pair() in align.c:340-372.
 * If q == q2 && e == e2, uses affine gap (z2); otherwise dual-affine (d2).
 *
 * @param query_length   Query segment length
 * @param query_seq      Encoded query segment
 * @param target_length  Target segment length
 * @param target_seq     Encoded target segment
 * @param score_matrix   5x5 scoring matrix
 * @param params         Algorithm parameters
 * @param align_bandwidth Bandwidth for alignment
 * @param end_bonus      End bonus (typically -1 for gap filling, opt->end_bonus for extensions)
 * @param zdrop          Z-drop threshold
 * @param ksw_flag       KSW2 flags (KSW_EZ_EXTZ_ONLY, KSW_EZ_RIGHT, KSW_EZ_REV_CIGAR, etc.)
 * @param result         [in/out] KSW2 result struct
 */
void ksw2_align_pair(
    int query_length, const uint8_t* query_seq,
    int target_length, const uint8_t* target_seq,
    const int8_t* score_matrix,
    const Minimap2Params& params,
    int align_bandwidth, int end_bonus, int zdrop, int ksw_flag,
    ksw_extz_t* result)
{
    if (params.gap_open1 == params.gap_open2 && params.gap_ext1 == params.gap_ext2) {
        Aligner<ALIGN_ALGORITHM_KSW2>::z2(
            nullptr, query_length, query_seq, target_length, target_seq, 5, score_matrix,
            static_cast<int8_t>(params.gap_open1),
            static_cast<int8_t>(params.gap_ext1),
            align_bandwidth, zdrop, end_bonus, ksw_flag, result);
    } else {
        Aligner<ALIGN_ALGORITHM_KSW2>::d2(
            nullptr, query_length, query_seq, target_length, target_seq, 5, score_matrix,
            static_cast<int8_t>(params.gap_open1),
            static_cast<int8_t>(params.gap_ext1),
            static_cast<int8_t>(params.gap_open2),
            static_cast<int8_t>(params.gap_ext2),
            align_bandwidth, zdrop, end_bonus, ksw_flag, result);
    }
}

/**
 * @brief Append KSW2 CIGAR ops to a CigarOp vector, merging adjacent same-op entries.
 */
void append_cigar(std::vector<CigarOp>& destination, int cigar_count, const uint32_t* cigar) {
    for (int cigar_idx = 0; cigar_idx < cigar_count; ++cigar_idx) {
        const uint32_t opcode = cigar[cigar_idx] & 0xf;
        const uint32_t op_length = cigar[cigar_idx] >> 4;
        if (!destination.empty() && destination.back().opcode == opcode) {
            destination.back().length += op_length;
        } else {
            destination.push_back({op_length, opcode});
        }
    }
}

/**
 * @brief Reverse an in-memory uint8_t sequence.
 */
void reverse_seq_inplace(uint8_t* sequence, int32_t seq_length) {
    std::reverse(sequence, sequence + seq_length);
}

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
 * Coordinates are derived from actual KSW2 alignment extent, not from the
 * padded window boundaries.
 */
void align_regions(
    std::vector<AlignRegion>& regions,
    const std::vector<Anchor>& anchors,
    const std::vector<uint8_t>& query_fwd,
    const std::vector<uint8_t>& query_rev,
    const std::vector<uint8_t>& target_encoded,
    const Minimap2Params& params)
{
    const auto score_matrix = generate_score_matrix(
        static_cast<int8_t>(params.match_score),
        static_cast<int8_t>(params.mismatch_pen),
        static_cast<int8_t>(params.sc_ambi));
    const int32_t query_length = static_cast<int32_t>(query_fwd.size());
    const int32_t target_length = static_cast<int32_t>(target_encoded.size());
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

        // Select the query strand
        const uint8_t* query_strand_base = region.is_reverse
            ? query_rev.data() : query_fwd.data();

        region.cigar.clear();
        region.dp_score = 0;
        ksw_extz_t ksw_result;
        std::memset(&ksw_result, 0, sizeof(ksw_result));

        // === 1. Left extension ===
        int32_t final_ref_start = seed_ref_start;
        int32_t final_query_start = seed_query_start;
        if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
            const int32_t left_query_len = seed_query_start - outer_query_start;
            const int32_t left_target_len = seed_ref_start - outer_ref_start;
            // Copy and reverse both query and target segments
            std::vector<uint8_t> query_buf(query_strand_base + outer_query_start,
                                           query_strand_base + seed_query_start);
            std::vector<uint8_t> target_buf(target_encoded.data() + outer_ref_start,
                                            target_encoded.data() + seed_ref_start);
            reverse_seq_inplace(query_buf.data(), left_query_len);
            reverse_seq_inplace(target_buf.data(), left_target_len);

            ksw_reset_extz(&ksw_result);
            ksw2_align_pair(left_query_len, query_buf.data(),
                            left_target_len, target_buf.data(),
                            score_matrix.data(), params, align_bandwidth,
                            params.end_bonus, params.zdrop,
                            KSW_EZ_EXTZ_ONLY | KSW_EZ_RIGHT | KSW_EZ_REV_CIGAR,
                            &ksw_result);
            if (ksw_result.n_cigar > 0) {
                append_cigar(region.cigar, ksw_result.n_cigar, ksw_result.cigar);
                region.dp_score += ksw_result.max;
            }
            const int32_t left_ref_offset = ksw_result.reach_end
                ? ksw_result.mqe_t + 1 : ksw_result.max_t + 1;
            final_ref_start = seed_ref_start - left_ref_offset;
            const int32_t left_query_offset = ksw_result.reach_end
                ? seed_query_start - outer_query_start : ksw_result.max_q + 1;
            final_query_start = seed_query_start - left_query_offset;
            if (ksw_result.cigar) {
                free(ksw_result.cigar);
                ksw_result.cigar = nullptr;
                ksw_result.n_cigar = 0;
                ksw_result.m_cigar = 0;
            }
        }

        // === 2. Gap filling (entire region between first and last seed) ===
        int32_t final_ref_end = seed_ref_start;
        int32_t final_query_end = seed_query_start;
        if (region.seed_count > 1) {
            const int32_t gap_query_len = seed_query_end - seed_query_start;
            const int32_t gap_target_len = seed_ref_end - seed_ref_start;
            if (gap_query_len > 0 && gap_target_len > 0) {
                const uint8_t* gap_query_seq = query_strand_base + seed_query_start;
                const uint8_t* gap_target_seq = target_encoded.data() + seed_ref_start;

                ksw_reset_extz(&ksw_result);
                ksw2_align_pair(gap_query_len, gap_query_seq,
                                gap_target_len, gap_target_seq,
                                score_matrix.data(), params, align_bandwidth,
                                -1, params.zdrop,
                                KSW_EZ_APPROX_MAX, &ksw_result);
                if (ksw_result.n_cigar > 0) {
                    append_cigar(region.cigar, ksw_result.n_cigar, ksw_result.cigar);
                }
                if (ksw_result.zdropped) {
                    final_ref_end = seed_ref_start + (ksw_result.max_t + 1);
                    final_query_end = seed_query_start + (ksw_result.max_q + 1);
                    region.dp_score += ksw_result.max;
                } else {
                    final_ref_end = seed_ref_end;
                    final_query_end = seed_query_end;
                    region.dp_score += ksw_result.score;
                }
                if (ksw_result.cigar) {
                    free(ksw_result.cigar);
                    ksw_result.cigar = nullptr;
                    ksw_result.n_cigar = 0;
                    ksw_result.m_cigar = 0;
                }
            }
        } else {
            final_ref_end = seed_ref_end;
            final_query_end = seed_query_end;
        }

        // === 3. Right extension ===
        if (!ksw_result.zdropped
            && seed_query_end < outer_query_end
            && seed_ref_end < outer_ref_end)
        {
            const int32_t right_query_len = outer_query_end - seed_query_end;
            const int32_t right_target_len = outer_ref_end - seed_ref_end;
            const uint8_t* right_query_seq = query_strand_base + seed_query_end;
            const uint8_t* right_target_seq = target_encoded.data() + seed_ref_end;

            ksw_reset_extz(&ksw_result);
            ksw2_align_pair(right_query_len, right_query_seq,
                            right_target_len, right_target_seq,
                            score_matrix.data(), params, align_bandwidth,
                            params.end_bonus, params.zdrop,
                            KSW_EZ_EXTZ_ONLY, &ksw_result);
            if (ksw_result.n_cigar > 0) {
                append_cigar(region.cigar, ksw_result.n_cigar, ksw_result.cigar);
                region.dp_score += ksw_result.max;
            }
            const int32_t right_ref_offset = ksw_result.reach_end
                ? ksw_result.mqe_t + 1 : ksw_result.max_t + 1;
            final_ref_end = seed_ref_end + right_ref_offset;
            const int32_t right_query_offset = ksw_result.reach_end
                ? outer_query_end - seed_query_end : ksw_result.max_q + 1;
            final_query_end = seed_query_end + right_query_offset;
            if (ksw_result.cigar) {
                free(ksw_result.cigar);
                ksw_result.cigar = nullptr;
                ksw_result.n_cigar = 0;
                ksw_result.m_cigar = 0;
            }
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
    const AlignRegion& region,
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
        .cigar                 = region.cigar,
    };
}

// ============================================================================
// PAF output — MatchResultCollection
// ============================================================================

/// CIGAR operation code to character, matching MM_CIGAR_STR "MIDNSHP=XB"
constexpr std::array<char, 10> CIGAR_OP_CHAR = {
    'M', 'I', 'D', 'N', 'S', 'H', 'P', '=', 'X', 'B'
};

/**
 * @brief Format a single CIGAR string from CigarOp vector.
 *
 * Each op is printed as "{length}{op_char}" using the same encoding as
 * minimap2's MM_CIGAR_STR (index 0='M', 1='I', 2='D', 7='=', 8='X', etc.).
 */
[[nodiscard]] std::string format_cigar(const std::vector<CigarOp>& cigar) {
    std::string cigar_string;
    cigar_string.reserve(cigar.size() * 5);
    for (const auto& cigar_op : cigar) {
        cigar_string += std::to_string(cigar_op.length);
        if (cigar_op.opcode < CIGAR_OP_CHAR.size()) {
            cigar_string += CIGAR_OP_CHAR[cigar_op.opcode];
        } else {
            cigar_string += '?';
        }
    }
    return cigar_string;
}

/**
 * @brief Wrapper around std::vector<MatchResult> providing PAF output formatting.
 *
 * Provides a print() method that outputs results in exact minimap2 PAF format,
 * reproducing mm_write_paf4() (format.c:427-460) with auxiliary tags from
 * write_tags() (format.c:399-425).
 */
struct MatchResultCollection {
    std::span<const MatchResult> results;

    /**
     * @brief Print alignment results in exact minimap2 PAF format.
     *
     * PAF line format (tab-separated):
     *   col 1:  query name             — user-provided
     *   col 2:  query sequence length
     *   col 3:  query start (0-based, closed)
     *   col 4:  query end   (0-based, open)
     *   col 5:  relative strand '+' or '-'
     *   col 6:  target name            — user-provided
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
            // Unmapped line: mimic mm_write_paf4 with r==NULL
            JustLog("{}\t0\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0", query_name);
            return;
        }

        for (const auto& match : results) {
            const char strand_char = match.is_reverse ? '-' : '+';
            const char type_char = match.is_primary ? 'P' : 'S';

            // 12 mandatory PAF columns
            std::string line = fmt::format(
                "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                query_name, match.query_len, match.query_start, match.query_end,
                strand_char, target_name, match.target_len, match.target_start,
                match.target_end, match.num_matches, match.block_length, match.mapq);

            // --- Auxiliary tags ---
            // When CIGAR is available (r->p != NULL in original), output NM, ms, AS, nn
            if (match.has_cigar && !match.cigar.empty()) {
                const int32_t edit_distance = std::max(0,
                    match.block_length - match.num_matches);
                line += fmt::format("\tNM:i:{}\tms:i:{}\tAS:i:{}\tnn:i:0",
                    edit_distance, match.dp_max_score, match.dp_score);
            }

            // tp:A — type (format.c:409, always output)
            line += fmt::format("\ttp:A:{}", type_char);

            // cm:i — seed count (format.c:409, always output)
            line += fmt::format("\tcm:i:{}", match.seed_count);

            // s1:i — chaining score (format.c:409, always output)
            line += fmt::format("\ts1:i:{}", match.chain_score);

            // s2:i — best secondary chaining score (only for primaries, format.c:410)
            if (match.is_primary) {
                line += fmt::format("\ts2:i:{}", match.secondary_chain_score);
            }

            // Divergence: de:f when CIGAR available, dv:f otherwise (format.c:411-422)
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

            // rl:i — repeat length (format.c:455, always 0 for us)
            line += "\trl:i:0";

            // cg:Z — CIGAR string (format.c:456-460, only when CIGAR available)
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
    const std::vector<MatchResult>& results,
    const std::string& query_name  = "query",
    const std::string& target_name = "target")
{
    const MatchResultCollection collection{std::span<const MatchResult>(results)};
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
 * @param query_seq           Query DNA sequence
 * @param target_seq          Target DNA sequence
 * @param query_minimizers    Pre-computed minimizers for the query
 * @param target_minimizers   Pre-computed minimizers for the target
 * @param params              Algorithm parameters (defaults match minimap2)
 * @return Vector of PAF-like alignment results
 */
[[nodiscard]] std::vector<MatchResult> minimap2_port(
    const std::vector<seq::dna4>& query_seq,
    const std::vector<seq::dna4>& target_seq,
    const std::vector<MinimapSketchData>& query_minimizers,
    const std::vector<MinimapSketchData>& target_minimizers,
    const Minimap2Params& params = Minimap2Params{})
{
    const int32_t query_len  = static_cast<int32_t>(query_seq.size());
    const int32_t target_len = static_cast<int32_t>(target_seq.size());

    JustLog("[minimap_port] Starting minimap2_port: query_len={}, target_len={}, "
            "query_minimizers={}, target_minimizers={}",
            query_len, target_len, query_minimizers.size(), target_minimizers.size());

    if (query_len == 0 || target_len == 0) {
        JustLog("[minimap_port] Empty input — returning no results");
        return {};
    }

    // ---- Phase 1: Build target index ----
    TargetIndex target_index;
    target_index.build(target_minimizers, target_len);

    // ---- Phase 2: Collect seed hits ----
    auto anchors = collect_seed_hits(
        query_minimizers, target_index, query_len, params.max_occ);

    if (anchors.empty()) {
        JustLog("[minimap_port] No seed hits found — returning no results");
        return {};
    }

    // ---- Phase 3: Chain seeds ----
    auto [chains, reordered_anchors] = chain_anchors_dp(anchors, params);

    if (chains.empty()) {
        JustLog("[minimap_port] No chains formed — returning no results");
        return {};
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
        auto query_fwd = encode_sequence(query_seq);
        auto query_rev_enc = reverse_complement(query_fwd);
        auto target_encoded = encode_sequence(target_seq);

        align_regions(regions, reordered_anchors,
                      query_fwd, query_rev_enc, target_encoded, params);

        // Re-classify after DP (like align_regs does in map.c:572-574)
        classify_primary_secondary(regions, params.mask_level, params.mask_len, sub_diff);
        filter_secondary_regions(regions, params.pri_ratio, min_diff, params.best_n);
    }

    // ---- Phase 8: Compute MAPQ ----
    compute_mapping_quality(regions, params, 0);

    // ---- Phase 9: Convert to MatchResult ----
    std::vector<MatchResult> results;
    results.reserve(regions.size());
    for (const auto& region : regions) {
        results.push_back(region_to_match_result(region, query_len, target_len));
    }

    JustLog("[minimap_port] Pipeline complete: {} alignment results", results.size());
    return results;
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

} // export
