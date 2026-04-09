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
 *   9. Return results as MatchResults (PAF-like records)
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
    int32_t k              = 15;     ///< k-mer size
    int32_t w              = 10;     ///< minimizer window
    int32_t max_gap        = 5000;   ///< max reference gap in chaining
    int32_t max_gap_ref    = -1;     ///< explicit ref gap (-1 = use max_gap)
    int32_t bw             = 500;    ///< bandwidth for chaining & alignment
    int32_t max_chain_skip = 25;     ///< max skip for early termination in DP
    int32_t max_chain_iter = 5000;   ///< max predecessors examined per anchor (map.c)
    int32_t min_cnt        = 3;      ///< min seeds per chain
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

constexpr int32_t CIGAR_MATCH     = 0;
constexpr int32_t CIGAR_INS       = 1;
constexpr int32_t CIGAR_DEL       = 2;
constexpr int32_t CIGAR_EQ        = 7;
constexpr int32_t CIGAR_X         = 8;

// ============================================================================
// Result structures
// ============================================================================

/**
 * @brief Single CIGAR operation: (length << 4) | op_code.
 */
struct CigarOp {
    uint32_t len;
    uint32_t op;
};

/**
 * @brief PAF-like alignment result.
 *
 * Maps to the 12 mandatory PAF columns plus alignment details.
 */
struct MatchResult {
    // PAF mandatory columns
    int32_t query_len       = 0;    ///< col 2: query sequence length
    int32_t query_start     = 0;    ///< col 3: query start (0-based, closed)
    int32_t query_end       = 0;    ///< col 4: query end (0-based, open)
    bool    is_reverse      = false;///< col 5: '+' or '-'
    int32_t target_len      = 0;    ///< col 7: target sequence length
    int32_t target_start    = 0;    ///< col 8: target start on original strand
    int32_t target_end      = 0;    ///< col 9: target end on original strand
    int32_t num_matches     = 0;    ///< col 10: number of residue matches (mlen)
    int32_t block_len       = 0;    ///< col 11: alignment block length (blen)
    int32_t mapq            = 0;    ///< col 12: mapping quality (0-255)

    // Additional alignment info
    int32_t chain_score     = 0;    ///< chaining score
    int32_t dp_score        = 0;    ///< DP alignment score
    int32_t dp_max          = 0;    ///< max scoring segment score (dp_max0 in original)
    int32_t n_seeds         = 0;    ///< number of seeds in chain
    int32_t subsc           = 0;    ///< best secondary chaining score against this primary
    float   divergence      = -1.f; ///< sequence divergence estimate
    bool    is_primary      = true; ///< primary mapping flag
    bool    has_cigar       = false;///< whether CIGAR alignment was performed
    std::vector<CigarOp> cigar;     ///< CIGAR string
};

// ============================================================================
// Internal data structures
// ============================================================================

/**
 * @brief Anchor for chaining. Equivalent to mm128_t in collect_seed_hits output.
 *
 * Layout mirrors minimap2 conventions:
 *   x: rev<<63 | rid<<32 | rpos
 *   y: seg_id<<48 | flags<<40 | q_span<<32 | qpos
 *
 * For our single-target case, rid is always 0.
 */
struct Anchor {
    uint64_t x = 0;
    uint64_t y = 0;

    [[nodiscard]] constexpr int32_t rpos() const { return static_cast<int32_t>(x); }
    [[nodiscard]] constexpr int32_t qpos() const { return static_cast<int32_t>(y); }
    [[nodiscard]] constexpr int32_t q_span() const {
        return static_cast<int32_t>((y >> 32) & 0xff);
    }
    [[nodiscard]] constexpr uint32_t rid() const {
        return static_cast<uint32_t>((x << 1) >> 33);
    }
    [[nodiscard]] constexpr bool rev() const { return (x >> 63) != 0; }
};

/**
 * @brief One chain of anchors, with metadata.
 *
 * Equivalent to one entry in the u[] array after chaining.
 */
struct Chain {
    int32_t score       = 0;    ///< chain score (upper 32 bits of u[])
    int32_t n_anchors   = 0;    ///< anchor count (lower 32 bits of u[])
    int32_t anchor_start= 0;    ///< start index in the reordered anchor array
};

/**
 * @brief Alignment region — equivalent to mm_reg1_t.
 *
 * Produced after mm_gen_regs, enriched through the pipeline.
 */
struct AlignRegion {
    int32_t id          = 0;
    int32_t parent      = -1;   ///< parent id (-1 = unset, ==id => primary)
    int32_t score       = 0;    ///< chain score
    int32_t score0      = 0;    ///< initial chain score (before merging)
    int32_t cnt         = 0;    ///< number of seeds
    int32_t anchor_start= 0;    ///< offset in anchor array

    int32_t qs = 0, qe = 0;    ///< query start/end
    int32_t rs = 0, re = 0;    ///< reference start/end
    int32_t rid         = 0;    ///< reference sequence id (0 in our case)
    bool    rev         = false;///< reverse strand

    int32_t mlen        = 0;    ///< seeded exact match length
    int32_t blen        = 0;    ///< seeded alignment block length
    int32_t subsc       = 0;    ///< best secondary score against this primary
    int32_t n_sub       = 0;    ///< number of suboptimal mappings

    // DP alignment results (filled after alignment phase)
    int32_t dp_score    = 0;
    int32_t dp_max      = 0;    ///< max scoring segment
    int32_t dp_max2     = 0;    ///< best secondary DP score
    bool    has_dp      = false;///< whether DP alignment was performed
    float   div         = -1.f; ///< divergence estimate

    int32_t mapq        = 0;

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
inline float fast_log2(float x) {
    uint32_t zi = std::bit_cast<uint32_t>(x);
    float log_2 = static_cast<float>(((zi >> 23) & 255) - 128);
    zi &= ~(255u << 23);
    zi += 127u << 23;
    float zf = std::bit_cast<float>(zi);
    log_2 += (-0.34484843f * zf + 2.02466578f) * zf - 0.67487759f;
    return log_2;
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
               int32_t tlen) {
        target_len = tlen;
        table.clear();
        table.reserve(target_minimizers.size());
        for (const auto& m : target_minimizers) {
            uint64_t hash_key = m.kmer;
            // Encode position as: (rid << 32) | (last_pos << 1) | strand
            // rid = 0 for single target
            uint64_t pos_enc = (static_cast<uint64_t>(0) << 32)
                             | (static_cast<uint64_t>(m.last_pos) << 1)
                             | static_cast<uint64_t>(m.strand);
            table[hash_key].push_back(pos_enc);
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
        auto it = table.find(kmer_hash);
        if (it == table.end()) return {};
        return std::span<const uint64_t>(it->second);
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
 *   a.x = rev<<63 | rid<<32 | rpos
 *   a.y = q_span<<32 | qpos
 *
 * Where for forward strand: rpos = target_pos, qpos = query_pos
 * For reverse strand: set rev bit, adjust qpos for reverse complement.
 *
 * @param query_minimizers Pre-computed query minimizers
 * @param target_index     Target index to search
 * @param query_len        Length of query sequence
 * @param max_occ          Max occurrences to keep (filter high-frequency seeds)
 * @return Sorted vector of anchors
 */
std::vector<Anchor> collect_seed_hits(
    const std::vector<MinimapSketchData>& query_minimizers,
    const TargetIndex& target_index,
    int32_t query_len,
    int32_t max_occ)
{
    std::vector<Anchor> anchors;
    anchors.reserve(query_minimizers.size() * 2);

    for (const auto& qm : query_minimizers) {
        auto hits = target_index.lookup(qm.kmer);
        if (hits.empty() || static_cast<int32_t>(hits.size()) > max_occ) continue;

        uint32_t q_pos_enc  = static_cast<uint32_t>((qm.last_pos << 1) | qm.strand);
        uint32_t q_span_val = static_cast<uint32_t>(qm.span);

        for (uint64_t r : hits) {
            int32_t rpos = static_cast<int32_t>(static_cast<uint32_t>(r) >> 1);
            Anchor a;

            bool same_strand = ((r & 1) == (q_pos_enc & 1));
            if (same_strand) {
                // Forward strand hit
                a.x = (r & 0xFFFFFFFF00000000ULL) | static_cast<uint32_t>(rpos);
                a.y = (static_cast<uint64_t>(q_span_val) << 32)
                    | static_cast<uint32_t>(q_pos_enc >> 1);
            } else {
                // Reverse strand hit
                a.x = (1ULL << 63) | (r & 0xFFFFFFFF00000000ULL)
                     | static_cast<uint32_t>(rpos);
                int32_t fwd_qpos = static_cast<int32_t>(q_pos_enc >> 1);
                int32_t rev_qpos = query_len - (fwd_qpos + 1 - static_cast<int32_t>(q_span_val)) - 1;
                a.y = (static_cast<uint64_t>(q_span_val) << 32)
                    | static_cast<uint32_t>(rev_qpos);
            }
            anchors.push_back(a);
        }
    }

    // Sort by reference position (x field), same as radix_sort_128x
    std::sort(anchors.begin(), anchors.end(),
              [](const Anchor& a, const Anchor& b) {
                  return a.x < b.x || (a.x == b.x && a.y < b.y);
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
 * The score is: min(q_span_j, min(dr, dq)) minus gap penalties.
 * Gap penalty = lin_pen + 0.5 * log_pen, where:
 *   lin_pen = chn_pen_gap * |dr-dq| + chn_pen_skip * min(dr,dq)
 *   log_pen = log2(|dr-dq| + 1)
 *
 * Returns INT32_MIN if the pair is invalid (too far, wrong order, etc.)
 */
inline int32_t compute_chaining_score(
    const Anchor& ai, const Anchor& aj,
    int32_t max_dist_x, int32_t max_dist_y, int32_t bw,
    float chn_pen_gap, float chn_pen_skip)
{
    int32_t dq = ai.qpos() - aj.qpos();
    if (dq <= 0 || dq > max_dist_x) return std::numeric_limits<int32_t>::min();

    int32_t dr = static_cast<int32_t>(ai.x - aj.x);
    if (dr == 0 || dq > max_dist_y) return std::numeric_limits<int32_t>::min();

    int32_t dd = dr > dq ? dr - dq : dq - dr;
    if (dd > bw) return std::numeric_limits<int32_t>::min();

    int32_t dg = dr < dq ? dr : dq;
    int32_t q_span = aj.q_span();
    int32_t sc = q_span < dg ? q_span : dg;

    if (dd != 0 || dg > q_span) {
        float lin_pen = chn_pen_gap * static_cast<float>(dd)
                      + chn_pen_skip * static_cast<float>(dg);
        float log_pen = (dd >= 1) ? fast_log2(static_cast<float>(dd + 1)) : 0.0f;
        sc -= static_cast<int32_t>(lin_pen + 0.5f * log_pen);
    }
    return sc;
}

/**
 * @brief Chain anchors using dynamic programming.
 *
 * Equivalent to mg_lchain_dp() in lchain.c:150-219.
 *
 * Algorithm:
 *   For each anchor i (sorted by ref position):
 *     1. Search backwards through prior anchors j in [st, i-1]
 *     2. Compute chaining score: comput_sc(i, j) + f[j]
 *     3. Track best predecessor and score
 *     4. Apply max_skip early termination
 *     5. Try the globally best anchor (max_ii) as fallback
 *
 *   Backtrack from highest-scoring anchors to extract chains.
 *
 * @param anchors  Sorted anchor array (modified: reordered on output)
 * @param params   Minimap2 parameters
 * @return Vector of chains with scores and anchor ranges
 */
std::pair<std::vector<Chain>, std::vector<Anchor>> chain_anchors_dp(
    std::vector<Anchor>& anchors,
    const Minimap2Params& params)
{
    const int64_t n = static_cast<int64_t>(anchors.size());
    if (n == 0) return {{}, {}};

    // map.c:722-723: chn_pen_gap = opt->chain_gap_scale * 0.01 * mi->k
    float chn_pen_gap  = params.chain_gap_scale * 0.01f * static_cast<float>(params.k);
    float chn_pen_skip = params.chain_skip_scale * 0.01f * static_cast<float>(params.k);
    int32_t max_dist_x = params.max_gap;
    int32_t max_dist_y = params.max_gap;
    int32_t bw         = params.bw;
    int32_t max_skip   = params.max_chain_skip;
    int32_t max_iter   = params.max_chain_iter;
    int32_t min_cnt    = params.min_cnt;
    int32_t min_sc     = params.min_chain_score;
    int32_t max_drop   = bw;

    if (max_dist_x < bw) max_dist_x = bw;
    if (max_dist_y < bw) max_dist_y = bw;

    // DP arrays
    std::vector<int32_t> f(n);      // best score ending at anchor i
    std::vector<int64_t> p(n, -1);  // predecessor index
    std::vector<int32_t> v(n);      // peak score
    std::vector<int32_t> t(n, 0);   // visited marker

    int64_t st = 0;
    int64_t max_ii = -1;

    // --- Forward DP pass ---
    // Equivalent to lchain.c lines 171-206
    for (int64_t i = 0; i < n; ++i) {
        int64_t max_j = -1;
        int32_t max_f = anchors[i].q_span();
        int32_t n_skip_cnt = 0;

        // Advance window start: skip anchors on different chromosomes or too far
        while (st < i && (anchors[i].x >> 32 != anchors[st].x >> 32 ||
               anchors[i].x > anchors[st].x + static_cast<uint64_t>(max_dist_x))) {
            ++st;
        }
        if (i - st > max_iter) st = i - max_iter;

        int64_t end_j = st;

        // Search backwards for best predecessor
        for (int64_t j = i - 1; j >= st; --j) {
            int32_t sc = compute_chaining_score(
                anchors[i], anchors[j],
                max_dist_x, max_dist_y, bw,
                chn_pen_gap, chn_pen_skip);
            if (sc == std::numeric_limits<int32_t>::min()) continue;

            sc += f[j];
            if (sc > max_f) {
                max_f = sc;
                max_j = j;
                if (n_skip_cnt > 0) --n_skip_cnt;
            } else if (t[j] == static_cast<int32_t>(i)) {
                if (++n_skip_cnt > max_skip) {
                    end_j = j;
                    break;
                }
            }
            if (p[j] >= 0) t[p[j]] = static_cast<int32_t>(i);
        }

        // Try the globally best anchor in the window (max_ii optimization)
        if (max_ii < 0 ||
            anchors[i].x - anchors[max_ii].x > static_cast<uint64_t>(max_dist_x)) {
            int32_t best = std::numeric_limits<int32_t>::min();
            max_ii = -1;
            for (int64_t j = i - 1; j >= st; --j) {
                if (best < f[j]) { best = f[j]; max_ii = j; }
            }
        }
        if (max_ii >= 0 && max_ii < end_j) {
            int32_t tmp = compute_chaining_score(
                anchors[i], anchors[max_ii],
                max_dist_x, max_dist_y, bw,
                chn_pen_gap, chn_pen_skip);
            if (tmp != std::numeric_limits<int32_t>::min() && max_f < tmp + f[max_ii]) {
                max_f = tmp + f[max_ii];
                max_j = max_ii;
            }
        }

        f[i] = max_f;
        p[i] = max_j;
        v[i] = (max_j >= 0 && v[max_j] > max_f) ? v[max_j] : max_f;

        if (max_ii < 0 ||
            (anchors[i].x - anchors[max_ii].x <= static_cast<uint64_t>(max_dist_x) &&
             f[max_ii] < f[i])) {
            max_ii = i;
        }
    }

    // --- Backtracking ---
    // Equivalent to mg_chain_backtrack() in lchain.c:29-78
    struct ScoredIdx {
        int32_t score;
        int64_t idx;
    };
    std::vector<ScoredIdx> z;
    z.reserve(n);
    for (int64_t i = 0; i < n; ++i) {
        if (f[i] >= min_sc)
            z.push_back({f[i], i});
    }

    std::sort(z.begin(), z.end(),
              [](const ScoredIdx& a, const ScoredIdx& b) { return a.score < b.score; });

    std::fill(t.begin(), t.end(), 0);

    std::vector<Chain> chains;
    std::vector<Anchor> reordered_anchors;
    std::vector<int64_t> chain_anchor_indices;

    for (int64_t k = static_cast<int64_t>(z.size()) - 1; k >= 0; --k) {
        if (t[z[k].idx] != 0) continue;

        chain_anchor_indices.clear();
        int64_t i;
        for (i = z[k].idx; i >= 0 && t[i] == 0; i = p[i]) {
            chain_anchor_indices.push_back(i);
            t[i] = 1;
        }

        int32_t sc = (i < 0) ? z[k].score : z[k].score - f[i];
        int32_t count = static_cast<int32_t>(chain_anchor_indices.size());

        if (sc >= min_sc && count >= min_cnt) {
            // Reverse to forward order
            std::reverse(chain_anchor_indices.begin(), chain_anchor_indices.end());

            Chain c;
            c.score = sc;
            c.n_anchors = count;
            c.anchor_start = static_cast<int32_t>(reordered_anchors.size());
            chains.push_back(c);

            for (int64_t idx : chain_anchor_indices) {
                reordered_anchors.push_back(anchors[idx]);
            }
        } else {
            // Unmark rejected chain
            for (int64_t idx : chain_anchor_indices) {
                t[idx] = 0;
            }
        }
    }

    JustLog("[minimap_port] Chaining: {} chains from {} anchors", chains.size(), n);
    return {std::move(chains), std::move(reordered_anchors)};
}

// ============================================================================
// Phase 4: Convert chains to alignment regions (mm_gen_regs equivalent)
// ============================================================================

/**
 * @brief Compute seeded match length (mlen) and block length (blen) for a region.
 *
 * Equivalent to mm_cal_fuzzy_len() in hit.c.
 *
 * mlen: approximate number of matching bases in seeded regions
 * blen: approximate total aligned length in seeded regions
 */
void compute_fuzzy_lengths(AlignRegion& r, const std::vector<Anchor>& anchors) {
    r.mlen = 0;
    r.blen = 0;
    if (r.cnt <= 0) return;

    int32_t as = r.anchor_start;
    r.mlen = r.blen = anchors[as].q_span();

    for (int32_t i = as + 1; i < as + r.cnt; ++i) {
        int32_t span = anchors[i].q_span();
        int32_t tl = static_cast<int32_t>(anchors[i].x) - static_cast<int32_t>(anchors[i-1].x);
        int32_t ql = static_cast<int32_t>(anchors[i].y) - static_cast<int32_t>(anchors[i-1].y);
        r.blen += (tl > ql) ? tl : ql;
        r.mlen += (tl > span && ql > span) ? span : ((tl < ql) ? tl : ql);
    }
}

/**
 * @brief Set alignment coordinates from seed positions.
 *
 * Equivalent to mm_reg_set_coor() in hit.c.
 * Derives qs, qe, rs, re from first and last anchor in the chain.
 */
void set_region_coordinates(
    AlignRegion& r,
    const std::vector<Anchor>& anchors,
    int32_t query_len)
{
    if (r.cnt <= 0) return;
    int32_t as = r.anchor_start;
    const Anchor& first = anchors[as];
    const Anchor& last  = anchors[as + r.cnt - 1];

    r.rev = first.rev();
    r.rid = static_cast<int32_t>(first.rid());

    // Reference coordinates
    int32_t span0 = first.q_span();
    r.rs = static_cast<int32_t>(first.x) + 1 - span0;
    r.re = static_cast<int32_t>(last.x) + 1;
    if (r.rs < 0) r.rs = 0;

    // Query coordinates
    int32_t qs_raw = static_cast<int32_t>(first.y) + 1 - span0;
    int32_t qe_raw = static_cast<int32_t>(last.y) + 1;
    if (qs_raw < 0) qs_raw = 0;

    if (!r.rev) {
        r.qs = qs_raw;
        r.qe = qe_raw;
    } else {
        r.qs = query_len - qe_raw;
        r.qe = query_len - qs_raw;
    }

    compute_fuzzy_lengths(r, anchors);
}

/**
 * @brief Convert chains to alignment regions.
 *
 * Equivalent to mm_gen_regs() in hit.c:54-90.
 *
 * Sorts chains by score (descending) and creates AlignRegion records
 * with coordinates derived from the anchor positions.
 */
std::vector<AlignRegion> generate_regions(
    std::vector<Chain>& chains,
    const std::vector<Anchor>& anchors,
    int32_t query_len)
{
    if (chains.empty()) return {};

    // Sort chains by score descending
    std::sort(chains.begin(), chains.end(),
              [](const Chain& a, const Chain& b) { return a.score > b.score; });

    std::vector<AlignRegion> regions(chains.size());
    for (size_t i = 0; i < chains.size(); ++i) {
        AlignRegion& r = regions[i];
        r.id = static_cast<int32_t>(i);
        r.parent = -1; // unset
        r.score = chains[i].score;
        r.score0 = chains[i].score;
        r.cnt = chains[i].n_anchors;
        r.anchor_start = chains[i].anchor_start;
        r.div = -1.0f;
        set_region_coordinates(r, anchors, query_len);
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
    int32_t n = static_cast<int32_t>(regions.size());
    if (n <= 0) return;

    for (int32_t i = 0; i < n; ++i) regions[i].id = i;

    // w[] holds indices of primary alignments
    std::vector<int32_t> primaries;
    primaries.push_back(0);
    regions[0].parent = 0;

    std::vector<uint64_t> cov_intervals;

    for (int32_t i = 1; i < n; ++i) {
        AlignRegion& ri = regions[i];
        int32_t si = ri.qs, ei = ri.qe;
        int32_t uncov_len = 0;

        // Compute coverage from existing primaries
        cov_intervals.clear();
        for (int32_t w_idx : primaries) {
            const AlignRegion& rp = regions[w_idx];
            int32_t sj = rp.qs, ej = rp.qe;
            if (ej <= si || sj >= ei) continue;
            sj = std::max(sj, si);
            ej = std::min(ej, ei);
            cov_intervals.push_back(
                (static_cast<uint64_t>(sj) << 32) | static_cast<uint32_t>(ej));
        }

        if (!cov_intervals.empty()) {
            std::sort(cov_intervals.begin(), cov_intervals.end());
            int32_t x = si;
            for (uint64_t c : cov_intervals) {
                int32_t cs = static_cast<int32_t>(c >> 32);
                int32_t ce = static_cast<int32_t>(c);
                if (cs > x) uncov_len += cs - x;
                x = std::max(x, ce);
            }
            if (ei > x) uncov_len += ei - x;
        }

        // Test against each primary
        bool is_secondary = false;
        for (int32_t w_idx : primaries) {
            AlignRegion& rp = regions[w_idx];
            int32_t sj = rp.qs, ej = rp.qe;
            if (ej <= si || sj >= ei) continue;

            int32_t min_span = std::min(ej - sj, ei - si);
            int32_t max_span = std::max(ej - sj, ei - si);
            int32_t ol = std::max(0, std::min(ei, ej) - std::max(si, sj));

            if (min_span > 0 && max_span > 0 &&
                (static_cast<float>(ol) / min_span -
                 static_cast<float>(uncov_len) / max_span) > mask_level &&
                uncov_len <= mask_len)
            {
                ri.parent = rp.parent >= 0 ? rp.parent : rp.id;
                rp.subsc = std::max(rp.subsc, ri.score);
                // Update dp_max2 for MAPQ (equivalent to hit.c:176)
                if (ri.has_dp && rp.has_dp &&
                    (rp.rid != ri.rid || rp.rs != ri.rs || rp.re != ri.re || ol != min_span)) {
                    rp.dp_max2 = std::max(rp.dp_max2, ri.dp_max);
                    if (rp.dp_max - ri.dp_max <= sub_diff) {
                        ++rp.n_sub;
                    }
                } else if (ri.cnt >= rp.cnt) {
                    ++rp.n_sub;
                }
                is_secondary = true;
                break;
            }
        }

        if (!is_secondary) {
            primaries.push_back(i);
            ri.parent = i;
            ri.n_sub = 0;
        }
    }

    JustLog("[minimap_port] Classification: {} primary, {} secondary",
            primaries.size(), n - static_cast<int32_t>(primaries.size()));
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
    if (regions.empty() || pri_ratio <= 0.0f) return;

    // Use index-based filtering to avoid reading from moved-from objects.
    // Mark each region as kept (true) or removed (false), then compact.
    int32_t n = static_cast<int32_t>(regions.size());
    int32_t n_2nd = 0;
    std::vector<bool> keep(n, false);

    for (int32_t i = 0; i < n; ++i) {
        const auto& r = regions[i];
        int32_t p_idx = r.parent;
        if (p_idx == r.id) {
            // Primary -- always keep
            keep[i] = true;
        } else if (p_idx >= 0 && p_idx < n) {
            const auto& parent = regions[p_idx];
            if ((r.score >= static_cast<int32_t>(parent.score * pri_ratio) ||
                 r.score + min_diff >= parent.score) && n_2nd < best_n)
            {
                // Not identical coordinates
                if (!(r.qs == parent.qs && r.qe == parent.qe &&
                      r.rid == parent.rid && r.rs == parent.rs && r.re == parent.re))
                {
                    keep[i] = true;
                    ++n_2nd;
                }
            }
        }
    }

    // Compact: move kept regions to front
    int32_t k = 0;
    for (int32_t i = 0; i < n; ++i) {
        if (keep[i]) {
            if (k < i) regions[k] = std::move(regions[i]);
            ++k;
        }
    }
    regions.resize(k);

    // Re-index
    for (int32_t i = 0; i < static_cast<int32_t>(regions.size()); ++i) {
        regions[i].id = i;
    }

    JustLog("[minimap_port] After secondary filtering: {} regions remain", regions.size());
}

// ============================================================================
// Phase 7: Base-level alignment — dual-affine gap DP (KSW2 equivalent)
// ============================================================================

/**
 * @brief Generate the 5x5 scoring matrix for DNA alignment.
 *
 * Matrix is 5x5 for {A, C, G, T, N} = {0, 1, 2, 3, 4}.
 * Diagonal = match_score, off-diagonal = -mismatch_pen.
 * Row/col 4 (N) scores 0 against everything.
 *
 * Equivalent to ksw_gen_simple_mat() in align.c:15-27.
 */
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
    std::array<int8_t, 25> mat{};
    int8_t a = match_score > 0 ? match_score : static_cast<int8_t>(-match_score);
    int8_t b = mismatch_pen > 0 ? static_cast<int8_t>(-mismatch_pen) : mismatch_pen;
    int8_t ambi = sc_ambi > 0 ? static_cast<int8_t>(-sc_ambi) : sc_ambi;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            mat[i * 5 + j] = (i == j) ? a : b;
        mat[i * 5 + 4] = ambi;
    }
    for (int j = 0; j < 5; ++j) mat[4 * 5 + j] = ambi;
    return mat;
}

/**
 * @brief Convert a dna4 base to 4-bit encoding (0=A, 1=C, 2=G, 3=T).
 */
inline uint8_t dna4_to_nt4(seq::dna4 base) {
    return static_cast<uint8_t>(base.to_rank());
}

/**
 * @brief Encode a dna4 sequence to uint8_t array.
 */
std::vector<uint8_t> encode_sequence(const std::vector<seq::dna4>& seq) {
    std::vector<uint8_t> encoded(seq.size());
    for (size_t i = 0; i < seq.size(); ++i) {
        encoded[i] = dna4_to_nt4(seq[i]);
    }
    return encoded;
}

/**
 * @brief Reverse complement an encoded sequence.
 */
std::vector<uint8_t> reverse_complement(const std::vector<uint8_t>& seq) {
    std::vector<uint8_t> rc(seq.size());
    for (size_t i = 0; i < seq.size(); ++i) {
        uint8_t c = seq[seq.size() - 1 - i];
        rc[i] = (c < 4) ? static_cast<uint8_t>(3 - c) : static_cast<uint8_t>(4);
    }
    return rc;
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
 * @param qlen     Query segment length
 * @param qseq     Encoded query segment
 * @param tlen     Target segment length
 * @param tseq     Encoded target segment
 * @param mat      5x5 scoring matrix
 * @param params   Algorithm parameters
 * @param w        Bandwidth for alignment
 * @param end_bonus End bonus (typically -1 for gap filling, opt->end_bonus for extensions)
 * @param zdrop    Z-drop threshold
 * @param ksw_flag KSW2 flags (KSW_EZ_EXTZ_ONLY, KSW_EZ_RIGHT, KSW_EZ_REV_CIGAR, etc.)
 * @param ez       [in/out] KSW2 result struct (cigar is allocated by KSW2, caller must free)
 */
void ksw2_align_pair(
    int qlen, const uint8_t* qseq,
    int tlen, const uint8_t* tseq,
    const int8_t* mat,
    const Minimap2Params& params,
    int w, int end_bonus, int zdrop, int ksw_flag,
    ksw_extz_t* ez)
{
    if (params.gap_open1 == params.gap_open2 && params.gap_ext1 == params.gap_ext2) {
        Aligner<ALIGN_ALGORITHM_KSW2>::z2(
            nullptr, qlen, qseq, tlen, tseq, 5, mat,
            static_cast<int8_t>(params.gap_open1),
            static_cast<int8_t>(params.gap_ext1),
            w, zdrop, end_bonus, ksw_flag, ez);
    } else {
        Aligner<ALIGN_ALGORITHM_KSW2>::d2(
            nullptr, qlen, qseq, tlen, tseq, 5, mat,
            static_cast<int8_t>(params.gap_open1),
            static_cast<int8_t>(params.gap_ext1),
            static_cast<int8_t>(params.gap_open2),
            static_cast<int8_t>(params.gap_ext2),
            w, zdrop, end_bonus, ksw_flag, ez);
    }
}

/**
 * @brief Convert KSW2 packed CIGAR to our CigarOp vector.
 *
 * KSW2 packs CIGAR as uint32_t: len<<4 | op.
 */
std::vector<CigarOp> ksw_cigar_to_ops(int n_cigar, const uint32_t* cigar) {
    std::vector<CigarOp> ops;
    ops.reserve(n_cigar);
    for (int i = 0; i < n_cigar; ++i) {
        ops.push_back({cigar[i] >> 4, cigar[i] & 0xf});
    }
    return ops;
}

/**
 * @brief Append KSW2 CIGAR ops to a CigarOp vector, merging adjacent same-op entries.
 */
void append_cigar(std::vector<CigarOp>& dst, int n_cigar, const uint32_t* cigar) {
    for (int i = 0; i < n_cigar; ++i) {
        uint32_t op  = cigar[i] & 0xf;
        uint32_t len = cigar[i] >> 4;
        if (!dst.empty() && dst.back().op == op) {
            dst.back().len += len;
        } else {
            dst.push_back({len, op});
        }
    }
}

/**
 * @brief Reverse an in-memory uint8_t sequence.
 */
void reverse_seq_inplace(uint8_t* seq, int32_t len) {
    for (int32_t i = 0; i < len / 2; ++i) {
        uint8_t tmp = seq[i];
        seq[i] = seq[len - 1 - i];
        seq[len - 1 - i] = tmp;
    }
}

/**
 * @brief Perform base-level alignment for each region using KSW2.
 *
 * Equivalent to align_regs() → mm_align_skeleton() → mm_align1() in align.c.
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
    auto mat = generate_score_matrix(
        static_cast<int8_t>(params.match_score),
        static_cast<int8_t>(params.mismatch_pen),
        static_cast<int8_t>(params.sc_ambi));
    int32_t qlen = static_cast<int32_t>(query_fwd.size());
    int32_t tlen = static_cast<int32_t>(target_encoded.size());
    int32_t bw = static_cast<int32_t>(params.bw * 1.5 + 1.0);

    for (auto& r : regions) {
        if (r.cnt <= 0) continue;

        int32_t as = r.anchor_start;
        const Anchor& first = anchors[as];
        const Anchor& last  = anchors[as + r.cnt - 1];

        // Inner seed boundaries (where seeds are)
        int32_t span0 = first.q_span();
        int32_t rs = static_cast<int32_t>(first.x) + 1 - span0;
        int32_t qs = static_cast<int32_t>(first.y) + 1 - span0;
        int32_t re = static_cast<int32_t>(last.x) + 1;
        int32_t qe = static_cast<int32_t>(last.y) + 1;
        if (rs < 0) rs = 0;
        if (qs < 0) qs = 0;

        // Outer boundaries: extend beyond seeds (align.c:708-762 simplified)
        int32_t rs0 = rs, qs0 = qs;
        int32_t re0 = re, qe0 = qe;
        if (qs > 0 && rs > 0) {
            int32_t l = qs < params.max_gap ? qs : params.max_gap;
            qs0 = qs - l;
            l += l * params.match_score > params.gap_open1 ? (l * params.match_score - params.gap_open1) / params.gap_ext1 : 0;
            l = l < params.max_gap ? l : params.max_gap;
            l = l < rs ? l : rs;
            rs0 = rs - l;
        }
        if (qe < qlen && re < tlen) {
            int32_t l = qlen - qe < params.max_gap ? qlen - qe : params.max_gap;
            qe0 = qe + l;
            l += l * params.match_score > params.gap_open1 ? (l * params.match_score - params.gap_open1) / params.gap_ext1 : 0;
            l = l < params.max_gap ? l : params.max_gap;
            l = l < tlen - re ? l : tlen - re;
            re0 = re + l;
        }
        if (qe0 > qlen) qe0 = qlen;

        // Select the query strand
        const uint8_t* qseq_base = r.rev ? query_rev.data() : query_fwd.data();

        r.cigar.clear();
        r.dp_score = 0;
        ksw_extz_t ez;
        std::memset(&ez, 0, sizeof(ez));

        // === 1. Left extension ===
        int32_t rs1 = rs, qs1 = qs;
        if (qs > qs0 && rs > rs0) {
            int32_t left_qlen = qs - qs0;
            int32_t left_tlen = rs - rs0;
            // Copy and reverse both query and target segments
            std::vector<uint8_t> qbuf(qseq_base + qs0, qseq_base + qs);
            std::vector<uint8_t> tbuf(target_encoded.data() + rs0, target_encoded.data() + rs);
            reverse_seq_inplace(qbuf.data(), left_qlen);
            reverse_seq_inplace(tbuf.data(), left_tlen);

            ksw_reset_extz(&ez);
            ksw2_align_pair(left_qlen, qbuf.data(), left_tlen, tbuf.data(),
                            mat.data(), params, bw,
                            params.end_bonus, params.zdrop,
                            KSW_EZ_EXTZ_ONLY | KSW_EZ_RIGHT | KSW_EZ_REV_CIGAR, &ez);
            if (ez.n_cigar > 0) {
                append_cigar(r.cigar, ez.n_cigar, ez.cigar);
                r.dp_score += ez.max;
            }
            rs1 = rs - (ez.reach_end ? ez.mqe_t + 1 : ez.max_t + 1);
            qs1 = qs - (ez.reach_end ? qs - qs0 : ez.max_q + 1);
            if (ez.cigar) { free(ez.cigar); ez.cigar = nullptr; ez.n_cigar = 0; ez.m_cigar = 0; }
        }

        // === 2. Gap filling (entire region between first and last seed) ===
        int32_t re1 = rs, qe1 = qs;
        if (r.cnt > 1) {
            int32_t gap_qlen = qe - qs;
            int32_t gap_tlen = re - rs;
            if (gap_qlen > 0 && gap_tlen > 0) {
                const uint8_t* gap_qseq = qseq_base + qs;
                const uint8_t* gap_tseq = target_encoded.data() + rs;

                ksw_reset_extz(&ez);
                ksw2_align_pair(gap_qlen, gap_qseq, gap_tlen, gap_tseq,
                                mat.data(), params, bw,
                                -1, params.zdrop,
                                KSW_EZ_APPROX_MAX, &ez);
                if (ez.n_cigar > 0) {
                    append_cigar(r.cigar, ez.n_cigar, ez.cigar);
                }
                if (ez.zdropped) {
                    re1 = rs + (ez.max_t + 1);
                    qe1 = qs + (ez.max_q + 1);
                    r.dp_score += ez.max;
                } else {
                    re1 = re;
                    qe1 = qe;
                    r.dp_score += ez.score;
                }
                if (ez.cigar) { free(ez.cigar); ez.cigar = nullptr; ez.n_cigar = 0; ez.m_cigar = 0; }
            }
        } else {
            re1 = re; qe1 = qe;
        }

        // === 3. Right extension ===
        if (!ez.zdropped && qe < qe0 && re < re0) {
            int32_t right_qlen = qe0 - qe;
            int32_t right_tlen = re0 - re;
            const uint8_t* right_qseq = qseq_base + qe;
            const uint8_t* right_tseq = target_encoded.data() + re;

            ksw_reset_extz(&ez);
            ksw2_align_pair(right_qlen, right_qseq, right_tlen, right_tseq,
                            mat.data(), params, bw,
                            params.end_bonus, params.zdrop,
                            KSW_EZ_EXTZ_ONLY, &ez);
            if (ez.n_cigar > 0) {
                append_cigar(r.cigar, ez.n_cigar, ez.cigar);
                r.dp_score += ez.max;
            }
            re1 = re + (ez.reach_end ? ez.mqe_t + 1 : ez.max_t + 1);
            qe1 = qe + (ez.reach_end ? qe0 - qe : ez.max_q + 1);
            if (ez.cigar) { free(ez.cigar); ez.cigar = nullptr; ez.n_cigar = 0; ez.m_cigar = 0; }
        }

        // Set final coordinates from alignment extent (align.c:897-899)
        r.rs = rs1;
        r.re = re1;
        if (!r.rev) {
            r.qs = qs1;
            r.qe = qe1;
        } else {
            r.qs = qlen - qe1;
            r.qe = qlen - qs1;
        }

        // Compute dp_max from CIGAR (simplified mm_update_extra)
        r.dp_max = r.dp_score;
        r.has_dp = true;

        // Compute mlen/blen from CIGAR
        int32_t n_match = 0, n_block = 0;
        for (const auto& op : r.cigar) {
            if (op.op == static_cast<uint32_t>(CIGAR_MATCH) ||
                op.op == static_cast<uint32_t>(CIGAR_EQ)) {
                n_match += static_cast<int32_t>(op.len);
                n_block += static_cast<int32_t>(op.len);
            } else if (op.op == static_cast<uint32_t>(CIGAR_X)) {
                n_block += static_cast<int32_t>(op.len);
            } else if (op.op == static_cast<uint32_t>(CIGAR_INS) ||
                       op.op == static_cast<uint32_t>(CIGAR_DEL)) {
                n_block += static_cast<int32_t>(op.len);
            }
        }
        if (n_match > 0) {
            r.mlen = n_match;
            r.blen = n_block > 0 ? n_block : r.blen;
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
 * @param k                 k-mer size
 * @param target_len        Target/reference sequence length
 */
void estimate_divergence_from_seeds(
    std::vector<AlignRegion>& regions,
    const std::vector<MinimapSketchData>& query_minimizers,
    const std::vector<Anchor>& anchors,
    int32_t query_len,
    int32_t k,
    int32_t target_len)
{
    if (query_minimizers.empty()) return;

    // Build sorted mini_pos array: stores last_pos values directly.
    // seed.c:127: mini_pos = q_span<<32 | q_pos>>1, where q_pos>>1 == last_pos.
    // esterr.c get_for_qpos on an anchor also resolves back to last_pos
    // for both strands (the reverse conversion cancels out), so mini_pos
    // simply contains the raw last_pos values sorted.
    std::vector<int32_t> mini_pos;
    mini_pos.reserve(query_minimizers.size());
    float sum_k = 0.0f;
    for (const auto& qm : query_minimizers) {
        mini_pos.push_back(static_cast<int32_t>(qm.last_pos));
        sum_k += static_cast<float>(qm.span);
    }
    std::sort(mini_pos.begin(), mini_pos.end());
    float avg_k = sum_k / static_cast<float>(query_minimizers.size());

    for (auto& r : regions) {
        r.div = -1.0f;
        if (r.cnt <= 0) continue;

        int32_t as = r.anchor_start;

        // Get forward-strand position of the first anchor in the chain.
        // esterr.c:48: uses r->rev? last anchor : first anchor.
        // get_for_qpos resolves to last_pos for both strands.
        const Anchor& ref_anchor = r.rev ? anchors[as + r.cnt - 1] : anchors[as];
        int32_t first_fwd_pos;
        {
            int32_t x = static_cast<int32_t>(ref_anchor.y);
            if (ref_anchor.rev()) {
                int32_t q_span = ref_anchor.q_span();
                x = query_len - 1 - (x + 1 - q_span);
            }
            first_fwd_pos = x;
        }

        // Binary search for first_fwd_pos in mini_pos
        auto it = std::lower_bound(mini_pos.begin(), mini_pos.end(), first_fwd_pos);
        if (it == mini_pos.end() || *it != first_fwd_pos) {
            continue; // Position not found — skip
        }
        int32_t st = static_cast<int32_t>(it - mini_pos.begin());
        int32_t en = st;

        // Scan forward matching chain seeds (esterr.c:55-60)
        int32_t n_match = 1;
        int32_t n = static_cast<int32_t>(mini_pos.size());
        for (int32_t seed_k = 1, j = st + 1; j < n && seed_k < r.cnt; ++j) {
            const Anchor& a_k = r.rev
                ? anchors[as + r.cnt - 1 - seed_k]
                : anchors[as + seed_k];
            int32_t fwd_pos;
            {
                int32_t x = static_cast<int32_t>(a_k.y);
                if (a_k.rev()) {
                    int32_t q_span = a_k.q_span();
                    x = query_len - 1 - (x + 1 - q_span);
                }
                fwd_pos = x;
            }
            if (fwd_pos == mini_pos[j]) {
                ++seed_k;
                en = j;
                ++n_match;
            }
        }

        // n_tot = span of mini_pos between first and last match (esterr.c:61)
        int32_t n_tot = en - st + 1;

        // Edge corrections (esterr.c:62-63)
        if (r.qs > avg_k && r.rs > avg_k) ++n_tot;
        if (query_len - r.qs > avg_k && target_len - r.re > avg_k) ++n_tot;

        // Divergence formula (esterr.c:64)
        r.div = (n_match >= n_tot)
                  ? 0.0f
                  : static_cast<float>(1.0 - std::pow(
                      static_cast<double>(n_match) / n_tot, 1.0 / avg_k));
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
 *   mapq = identity * pen_cm * 40 * (1 - x^2) * ln(dp_max / match_sc)
 *   mapq -= 4.343 * ln(n_sub + 1)
 *   mapq = clamp(mapq, 0, 60)
 *
 * Where x = dp_max2 * subsc / (dp_max * score0)
 */
void compute_mapping_quality(
    std::vector<AlignRegion>& regions,
    const Minimap2Params& params,
    int32_t rep_len)
{
    constexpr float q_coef = 40.0f;
    int32_t match_sc = params.match_score;
    int32_t min_chain_sc = params.min_chain_score;

    // Compute uniqueness ratio
    int64_t sum_sc = 0;
    for (const auto& r : regions) {
        if (r.parent == r.id) sum_sc += r.score;
    }
    // hit.c:436: uniq_ratio = (float)sum_sc / (sum_sc + rep_len)
    float uniq_ratio = static_cast<float>(sum_sc) /
                       static_cast<float>(sum_sc + rep_len);

    for (auto& r : regions) {
        if (r.parent != r.id) {
            r.mapq = 0; // secondaries get 0
            continue;
        }

        // Primary mapping
        float pen_s1 = (r.score > 100 ? 1.0f : 0.01f * r.score) * uniq_ratio;
        float pen_cm = r.cnt > 10 ? 1.0f : 0.1f * r.cnt;
        pen_cm = std::min(pen_s1, pen_cm);

        int32_t subsc = std::max(r.subsc, min_chain_sc);

        int32_t mapq_val;
        // hit.c:447-467: three branches based on DP availability
        if (r.has_dp && r.dp_max2 > 0 && r.dp_max > 0) {
            // Formula A: with DP scores (hit.c:448-457)
            float identity = (r.blen > 0) ? static_cast<float>(r.mlen) / r.blen : 0.f;
            float x = static_cast<float>(r.dp_max2) * subsc
                    / (static_cast<float>(r.dp_max) * r.score0);
            mapq_val = static_cast<int32_t>(
                identity * pen_cm * q_coef * (1.0f - x * x)
                * std::log(static_cast<float>(r.dp_max) / match_sc));

            // BWA-MEM-like alternative (hit.c:455)
            int32_t mapq_alt = static_cast<int32_t>(
                6.02f * identity * identity
                * (r.dp_max - r.dp_max2) / match_sc + 0.499f);
            mapq_val = std::min(mapq_val, mapq_alt);
        } else {
            // No dp_max2 available: use chain score formula (hit.c:460-467)
            float x = static_cast<float>(subsc) / r.score0;
            if (r.has_dp) {
                float identity = (r.blen > 0) ? static_cast<float>(r.mlen) / r.blen : 0.f;
                mapq_val = static_cast<int32_t>(
                    identity * pen_cm * q_coef * (1.0f - x)
                    * std::log(static_cast<float>(r.dp_max) / match_sc));
            } else {
                // No DP at all — use chain score (hit.c:466)
                mapq_val = static_cast<int32_t>(
                    pen_cm * q_coef * (1.0f - x)
                    * std::log(static_cast<float>(r.score)));
            }
        }

        // Subtract n_sub penalty (hit.c:469)
        mapq_val -= static_cast<int32_t>(4.343f * std::log(static_cast<float>(r.n_sub + 1)) + 0.499f);
        mapq_val = std::max(mapq_val, 0);
        r.mapq = std::min(mapq_val, 60);

        // Ensure non-zero if clearly best (hit.c:472)
        if (r.has_dp && r.dp_max > r.dp_max2 && r.mapq == 0) {
            r.mapq = 1;
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
MatchResult region_to_match_result(
    const AlignRegion& r,
    int32_t query_len,
    int32_t target_len)
{
    MatchResult m;
    m.query_len    = query_len;
    m.query_start  = r.qs;
    m.query_end    = r.qe;
    m.is_reverse   = r.rev;
    m.target_len   = target_len;
    m.target_start = r.rs;
    m.target_end   = r.re;
    m.num_matches  = r.mlen;
    m.block_len    = r.blen;
    m.mapq         = r.mapq;
    m.chain_score  = r.score;
    m.dp_score     = r.dp_score;
    m.dp_max       = r.dp_max;
    m.n_seeds      = r.cnt;
    m.subsc        = r.subsc;
    m.divergence   = r.div;
    m.is_primary   = (r.parent == r.id);
    m.has_cigar    = r.has_dp;
    m.cigar        = r.cigar;
    return m;
}

// ============================================================================
// PAF output — print_match_results
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
std::string format_cigar(const std::vector<CigarOp>& cigar) {
    std::string s;
    s.reserve(cigar.size() * 5);
    for (const auto& op : cigar) {
        s += std::to_string(op.len);
        if (op.op < CIGAR_OP_CHAR.size()) {
            s += CIGAR_OP_CHAR[op.op];
        } else {
            s += '?';
        }
    }
    return s;
}

/**
 * @brief Print alignment results in exact minimap2 PAF format.
 *
 * Reproduces the output of mm_write_paf4() (format.c:427-460) together with
 * the auxiliary tags from write_tags() (format.c:399-425).
 *
 * PAF line format (tab-separated):
 *   col 1:  query name             — we use "query" as placeholder
 *   col 2:  query sequence length
 *   col 3:  query start (0-based, closed)
 *   col 4:  query end   (0-based, open)
 *   col 5:  relative strand '+' or '-'
 *   col 6:  target name            — we use "target" as placeholder
 *   col 7:  target sequence length
 *   col 8:  target start on original strand (0-based)
 *   col 9:  target end   on original strand (0-based)
 *   col 10: number of residue matches  (mlen)
 *   col 11: alignment block length     (blen)
 *   col 12: mapping quality (0-255)
 *
 * Auxiliary tags (same key-value pairs as minimap2):
 *   NM:i  — edit distance (blen - mlen, approximation without n_ambi)
 *   ms:i  — max scoring segment DP score (dp_max)
 *   AS:i  — DP alignment score (dp_score)
 *   tp:A  — alignment type: 'P' = primary, 'S' = secondary
 *   cm:i  — number of minimizer/seed hits on the chain
 *   s1:i  — chaining score
 *   s2:i  — best secondary chaining score (only for primary hits)
 *   dv:f  — approximate per-base sequence divergence
 *   cg:Z  — CIGAR string (if alignment was performed)
 *
 * @param results         Vector of alignment results to print
 * @param query_name      Query sequence name (col 1), defaults to "query"
 * @param target_name     Target sequence name (col 6), defaults to "target"
 */
void print_match_results(
    const std::vector<MatchResult>& results,
    const std::string& query_name  = "query",
    const std::string& target_name = "target")
{
    if (results.empty()) {
        // Unmapped line: mimic mm_write_paf4 with r==NULL
        // Format: qname\tqlen\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0
        JustLog("{}\t0\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0", query_name);
        return;
    }

    for (const auto& m : results) {
        char strand_char = m.is_reverse ? '-' : '+';
        char tp = m.is_primary ? 'P' : 'S';

        // Build the full PAF line — format matches mm_write_paf4 + write_tags
        std::string line;
        line.reserve(256);

        // 12 mandatory PAF columns
        line += query_name;
        line += '\t'; line += std::to_string(m.query_len);
        line += '\t'; line += std::to_string(m.query_start);
        line += '\t'; line += std::to_string(m.query_end);
        line += '\t'; line += strand_char;
        line += '\t'; line += target_name;
        line += '\t'; line += std::to_string(m.target_len);
        line += '\t'; line += std::to_string(m.target_start);
        line += '\t'; line += std::to_string(m.target_end);
        line += '\t'; line += std::to_string(m.num_matches);
        line += '\t'; line += std::to_string(m.block_len);
        line += '\t'; line += std::to_string(m.mapq);

        // --- Auxiliary tags ---
        // When CIGAR is available (r->p != NULL in original), output NM, ms, AS, nn
        if (m.has_cigar && !m.cigar.empty()) {
            int32_t nm = m.block_len - m.num_matches;
            if (nm < 0) nm = 0;
            line += "\tNM:i:"; line += std::to_string(nm);
            line += "\tms:i:"; line += std::to_string(m.dp_max);
            line += "\tAS:i:"; line += std::to_string(m.dp_score);
            line += "\tnn:i:0";
        }

        // tp:A — type (format.c:409, always output)
        line += "\ttp:A:"; line += tp;

        // cm:i — seed count (format.c:409, always output)
        line += "\tcm:i:"; line += std::to_string(m.n_seeds);

        // s1:i — chaining score (format.c:409, always output)
        line += "\ts1:i:"; line += std::to_string(m.chain_score);

        // s2:i — best secondary chaining score (only for primaries, format.c:410)
        if (m.is_primary) {
            line += "\ts2:i:"; line += std::to_string(m.subsc);
        }

        // Divergence: de:f when CIGAR available, dv:f otherwise (format.c:411-422)
        if (m.divergence >= 0.0f && m.divergence <= 1.0f) {
            const char* tag = (m.has_cigar && !m.cigar.empty()) ? "\tde:f:" : "\tdv:f:";
            if (m.divergence == 0.0f) {
                line += tag; line += '0';
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(m.divergence));
                line += tag; line += buf;
            }
        }

        // rl:i — repeat length (format.c:455, always 0 for us)
        line += "\trl:i:0";

        // cg:Z — CIGAR string (format.c:456-460, only when CIGAR available and requested)
        if (m.has_cigar && !m.cigar.empty()) {
            line += "\tcg:Z:";
            line += format_cigar(m.cigar);
        }

        JustLog("{}", line);
    }
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
std::vector<MatchResult> minimap2_port(
    const std::vector<seq::dna4>& query_seq,
    const std::vector<seq::dna4>& target_seq,
    const std::vector<MinimapSketchData>& query_minimizers,
    const std::vector<MinimapSketchData>& target_minimizers,
    const Minimap2Params& params = Minimap2Params{})
{
    int32_t query_len  = static_cast<int32_t>(query_seq.size());
    int32_t target_len = static_cast<int32_t>(target_seq.size());

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
    int32_t sub_diff = params.match_score * 2 + params.mismatch_pen;
    classify_primary_secondary(regions, params.mask_level, params.mask_len, sub_diff);
    int32_t min_diff = params.k * 2;
    filter_secondary_regions(regions, params.pri_ratio, min_diff, params.best_n);

    // ---- Phase 6: Estimate divergence from seeds (mm_est_err equivalent) ----
    // This is done BEFORE alignment (map.c:783) and provides dv:f tag
    estimate_divergence_from_seeds(regions, query_minimizers, reordered_anchors,
                                   query_len, params.k, target_len);

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
    for (const auto& r : regions) {
        results.push_back(region_to_match_result(r, query_len, target_len));
    }

    JustLog("[minimap_port] Pipeline complete: {} alignment results", results.size());
    return results;
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

} // export
