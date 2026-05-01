/**
 * @file chaining_utils.cppm
 * @brief Shared types, scoring functions, and backtracking for all chaining algorithms.
 *
 * Single source of truth for:
 *   - Anchor, Chain, ChainingParams types
 *   - fast_log2 scalar approximation
 *   - compute_chaining_score (full, for scalar/SIMD scalar fallback)
 *   - compute_chaining_score_simple (simplified, for RMQ path)
 *   - backtrack_chains (shared by all three chaining algorithms)
 *
 * All three chaining modules (scalar, SIMD, RMQ) import this module.
 * minimap_port.cppm also imports this for Anchor/Chain types.
 */
module;

#include "common.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <bit>
#include <span>
#include <vector>
#include <algorithm>
#include <limits>

export module bioseq_sequences.chaining.utils;

import mlogging;
import context;

export {
namespace bioseq::chaining {

// ============================================================================
// Anchor and Chain types — moved from minimap_port.cppm
// ============================================================================

/// Bitfield data for anchor reference (x) component.
///
/// Layout mirrors minimap2 mm128_t.x conventions:
///   x: is_reverse<<63 | reference_id<<32 | ref_position
///
/// For our single-target case, reference_id is always 0.
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

/// Bitfield data for anchor query (y) component.
///
/// Layout mirrors minimap2 mm128_t.y conventions:
///   y: segment_id<<48 | flags<<40 | query_span<<32 | query_position
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

/// Anchor for chaining. Equivalent to mm128_t in collect_seed_hits output.
///
/// Uses unions to provide both clean bitfield access (ref/query structs) and
/// raw uint64_t access (ref_bits/query_bits) for sorting and arithmetic.
///
/// The unions previously defaulted to `= 0`. This forced a full 16 B
/// zero-store on every `Anchor anchor;` declaration — even though
/// `collect_seed_hits` immediately overwrote all four nested fields in
/// both code branches (see `minimap_port.cppm:619-643`). The zero-store
/// is a dead write the compiler can't elide because the writes go
/// through a *different* union member (`anchor.ref.ref_position` vs the
/// `ref_bits = 0` raw u64). With ~500-1500 anchors per pair and 400
/// pairs per bench section, the dead stores added up to ~2-4 ms per
/// section. Removed per proposal-2 §N3 — callers must write every
/// field they read, which is the existing behaviour.
struct Anchor {
    union {
        AnchorRefFields ref;
        uint64_t ref_bits;    ///< Raw value for sorting/arithmetic on reference data
    };
    union {
        AnchorQueryFields query;
        uint64_t query_bits;  ///< Raw value for sorting/arithmetic on query data
    };
};

/// One chain of anchors, with metadata.
///
/// Equivalent to one entry in the u[] array after chaining.
struct Chain {
    int32_t score        = 0;    ///< chain score (upper 32 bits of u[])
    int32_t anchor_count = 0;    ///< anchor count (lower 32 bits of u[])
    int32_t anchor_start = 0;    ///< start index in the reordered anchor array
};

// ============================================================================
// Chaining parameters
// ============================================================================

/// Algorithm selection for chaining.
enum class ChainingMode : uint8_t {
    SCALAR_DP,   ///< Original O(n * max_iter) scalar DP
    SIMD_DP,     ///< SIMD-vectorized DP (same algorithm, faster inner loop)
    RMQ,         ///< AVL+RMQ tree-based O(n log n)
};

/// Parameters controlling anchor chaining behavior.
///
/// Extracted from Minimap2Params -- only the chaining-relevant subset.
/// The full Minimap2Params in minimap_port.cppm derives these via to_chaining_params().
struct ChainingParams {
    int32_t kmer_size         = 15;
    int32_t max_gap           = 5000;
    int32_t bandwidth         = 500;
    int32_t max_chain_skip    = 25;
    int32_t max_chain_iter    = 5000;
    int32_t min_seed_count    = 3;
    int32_t min_chain_score   = 40;
    float   chain_gap_scale   = 0.8f;
    float   chain_skip_scale  = 0.0f;

    ChainingMode chaining_mode = ChainingMode::SIMD_DP;

    /// Derived penalty values. Call prepare() after setting scale values.
    float   gap_penalty       = 0.0f;   ///< chain_gap_scale * 0.01 * kmer_size
    float   skip_penalty      = 0.0f;   ///< chain_skip_scale * 0.01 * kmer_size

    /// Compute derived values from scale parameters.
    void prepare() {
        gap_penalty  = chain_gap_scale  * 0.01f * static_cast<float>(kmer_size);
        skip_penalty = chain_skip_scale * 0.01f * static_cast<float>(kmer_size);
    }
};

// ============================================================================
// Fast log2 approximation — matches minimap2's mg_log2 (mmpriv.h:139-147)
// ============================================================================

/// Fast log2 approximation using IEEE 754 float bit tricks.
///
/// Accurate to ~3 significant figures for input >= 2.
/// Matches minimap2 mg_log2() used in gap penalty computation.
///
/// @param input  Float value (must be >= 2 for accuracy)
/// @return Approximate log2(input)
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
// Chaining score functions
// ============================================================================

/// Compute chaining score between two anchors.
///
/// Equivalent to comput_sc() in lchain.c:115-140.
/// Score = min(predecessor_span, gap_length) - gap_penalties.
///
/// ASSUMPTION: Both anchors must be on the same chromosome. The window-start
/// advancement loop in the DP outer loop filters cross-chromosome pairs before
/// they reach this function. The ref_distance computation (int32 truncation of
/// 64-bit ref_bits subtraction) would produce garbage for different-chromosome pairs.
///
/// @param current_anchor      Anchor i (the one we are scoring)
/// @param predecessor_anchor  Anchor j (candidate predecessor)
/// @param max_ref_dist        Maximum reference distance (max_dist_x)
/// @param max_query_dist      Maximum query distance (max_dist_y)
/// @param chain_bandwidth     Bandwidth for diagonal deviation
/// @param gap_penalty         Precomputed: chain_gap_scale * 0.01 * kmer_size
/// @param skip_penalty        Precomputed: chain_skip_scale * 0.01 * kmer_size
/// @return Score, or INT32_MIN if the pair is invalid
[[nodiscard]] inline int32_t compute_chaining_score(
    const Anchor& current_anchor, const Anchor& predecessor_anchor,
    int32_t max_ref_dist, int32_t max_query_dist, int32_t chain_bandwidth,
    float gap_penalty, float skip_penalty)
{
    // The previous form had three sequential `if (...) return INT32_MIN;`
    // branches. Each one was a hard branch on the validity check
    // followed by a separate compute step, and the caller's `continue`
    // logic added a fourth `cmp` against INT32_MIN. With ~2500 calls per
    // chain × 400 pairs = ~1M chained branches per section, the branch
    // misprediction cost is real even when the inner arithmetic is
    // cheap. Restructure: compute every dependent value up-front and
    // combine the validity checks into one OR'd predicate. A single
    // branch + a single cmov-friendly select-on-INT32_MIN path replaces
    // the three early-exits. Per proposal-2 §N4.
    const int32_t query_distance = static_cast<int32_t>(current_anchor.query.query_position)
                                 - static_cast<int32_t>(predecessor_anchor.query.query_position);
    const int32_t ref_distance = static_cast<int32_t>(
        current_anchor.ref_bits - predecessor_anchor.ref_bits);
    const int32_t diagonal_deviation = (ref_distance > query_distance)
        ? ref_distance - query_distance
        : query_distance - ref_distance;

    const bool invalid =
        (query_distance <= 0)
      | (query_distance > max_ref_dist)
      | (ref_distance == 0)
      | (query_distance > max_query_dist)
      | (diagonal_deviation > chain_bandwidth);
    if (invalid) [[unlikely]] {
        return std::numeric_limits<int32_t>::min();
    }

    const int32_t gap_length = (ref_distance < query_distance)
        ? ref_distance : query_distance;
    const int32_t predecessor_span = static_cast<int32_t>(
        predecessor_anchor.query.query_span);
    int32_t score = (predecessor_span < gap_length) ? predecessor_span : gap_length;

    if (diagonal_deviation != 0 || gap_length > predecessor_span) {
        const float linear_penalty = gap_penalty * static_cast<float>(diagonal_deviation)
                                   + skip_penalty * static_cast<float>(gap_length);
        const float logarithmic_penalty = (diagonal_deviation >= 1)
            ? fast_log2(static_cast<float>(diagonal_deviation + 1))
            : 0.0f;
        score -= static_cast<int32_t>(linear_penalty + 0.5f * logarithmic_penalty);
    }
    return score;
}

/// Simplified chaining score for the RMQ path.
///
/// Equivalent to comput_sc_simple() in lchain.c:351-367.
/// Does not check segment IDs or cDNA mode.
/// Reports whether the score is "exact" (no gap penalty applied)
/// and the diagonal width.
///
/// IMPORTANT: The penalty trigger condition is `diagonal_deviation != 0 || query_distance > predecessor_span`.
/// This uses raw query_distance, NOT gap_length = min(ref_distance, query_distance).
/// This differs from the full compute_chaining_score which uses gap_length > predecessor_span.
///
/// ASSUMPTION: Both anchors must be on the same chromosome. The tree pruning logic
/// guarantees this.
///
/// @param current_anchor      Anchor i (the one we are scoring)
/// @param predecessor_anchor  Anchor j (candidate predecessor)
/// @param gap_penalty         Precomputed: chain_gap_scale * 0.01 * kmer_size
/// @param skip_penalty        Precomputed: chain_skip_scale * 0.01 * kmer_size
/// @param[out] is_exact_match True if diagonal_deviation==0 and gap_length<=predecessor_span
/// @param[out] diagonal_width The diagonal deviation |ref_distance - query_distance|
/// @return Score value
[[nodiscard]] inline int32_t compute_chaining_score_simple(
    const Anchor& current_anchor,
    const Anchor& predecessor_anchor,
    float gap_penalty,
    float skip_penalty,
    bool& is_exact_match,
    int32_t& diagonal_width)
{
    const int32_t query_distance = static_cast<int32_t>(current_anchor.query.query_position)
                                 - static_cast<int32_t>(predecessor_anchor.query.query_position);
    const int32_t ref_distance = static_cast<int32_t>(
        current_anchor.ref_bits - predecessor_anchor.ref_bits);
    const int32_t diagonal_deviation = (ref_distance > query_distance)
        ? (ref_distance - query_distance) : (query_distance - ref_distance);
    diagonal_width = diagonal_deviation;
    const int32_t gap_length = (ref_distance < query_distance) ? ref_distance : query_distance;
    const int32_t predecessor_span = static_cast<int32_t>(predecessor_anchor.query.query_span);
    int32_t score = (predecessor_span < gap_length) ? predecessor_span : gap_length;
    is_exact_match = (diagonal_deviation == 0 && gap_length <= predecessor_span);
    if (diagonal_deviation != 0 || query_distance > predecessor_span) {
        const float linear_penalty = gap_penalty * static_cast<float>(diagonal_deviation)
                                   + skip_penalty * static_cast<float>(gap_length);
        const float logarithmic_penalty = (diagonal_deviation >= 1)
            ? fast_log2(static_cast<float>(diagonal_deviation + 1)) : 0.0f;
        score -= static_cast<int32_t>(linear_penalty + 0.5f * logarithmic_penalty);
    }
    return score;
}

// ============================================================================
// Shared backtracking and chain extraction
// ============================================================================

/// Extract chains from DP arrays via backtracking.
///
/// Equivalent to mg_chain_backtrack() + compact_a() in lchain.c.
/// Processes anchors from highest to lowest score, tracing predecessor
/// chains and filtering by min_score and min_seed_count.
///
/// CRITICAL: For rejected chains (score or count too low), visited marks
/// are NOT cleared. The C code (lchain.c:43,55) deliberately leaves
/// t[i]=1 for rejected chains to prevent re-tracing through already-explored
/// anchors. The previous minimap_port.cppm (lines 693-698) incorrectly unmarked
/// rejected chains -- this function fixes that behavior.
///
/// @param anchors          Original (AoS) anchor array
/// @param best_scores      f[i] -- best chain score ending at anchor i
/// @param predecessors     p[i] -- predecessor index (-1 if chain start)
/// @param peak_scores      v[i] -- peak score along the chain through i
/// @param min_score        Minimum chain score to keep
/// @param min_seed_count   Minimum number of anchors per chain
/// @return Pair of (chains, reordered_anchors)
[[nodiscard]] inline std::pair<std::vector<Chain>, std::vector<Anchor>> backtrack_chains(
    const std::vector<Anchor>& anchors,
    std::span<const int32_t> best_scores,
    std::span<const int64_t> predecessors,
    std::span<const int32_t> peak_scores,
    int32_t min_score,
    int32_t min_seed_count)
{
    (void)peak_scores;  // upstream keeps it for parity with lchain.c; unused here
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());

    // Build scored index array, filter by min_score
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

    // Sort by score ascending so we process highest-scoring chains last->first
    std::sort(scored_indices.begin(), scored_indices.end(),
              [](const ScoredAnchorIndex& left, const ScoredAnchorIndex& right) {
                  return left.score < right.score;
              });

    // `visited` was previously a `std::vector<int32_t>(anchor_count, 0)` —
    // a fresh malloc + zero-fill on every chaining call. Move into the
    // chain arena so the per-pair allocator traffic disappears. The
    // chain arena's scope is owned by the calling chain_anchors_*; the
    // span lives until that scope drops, which is after this function
    // returns to the caller — safe.
    auto& bt_ctx = context::current();
    auto visited = bt_ctx.alloc_chain_array<int32_t>(static_cast<std::size_t>(anchor_count));
    std::memset(visited.data(), 0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));
    std::vector<Chain> chains;
    std::vector<Anchor> reordered_anchors;
    std::vector<int64_t> chain_anchor_indices;

    // Process from highest to lowest score
    for (int64_t rank_idx = static_cast<int64_t>(scored_indices.size()) - 1;
         rank_idx >= 0; --rank_idx)
    {
        // NOTE: Do NOT skip candidates where visited[candidate] != 0.
        // The trace naturally stops at visited nodes. (lchain.c:35-56)
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
        }
        // CRITICAL: Do NOT unmark visited[] for rejected chains.
        // This prevents re-tracing through already-explored anchors.
        // (lchain.c:43,55 deliberately leaves t[i]=1)
    }

    JustLog("[chaining] Backtracking: {} chains from {} anchors", chains.size(), anchor_count);
    return {std::move(chains), std::move(reordered_anchors)};
}

}  // namespace bioseq::chaining
}  // export
