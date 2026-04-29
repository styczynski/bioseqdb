/**
 * @file chaining_simd.cppm
 * @brief SIMD-accelerated DP chaining using Highway.
 *
 * Same algorithm as scalar DP chaining, but the inner predecessor search
 * loop is vectorized: instead of evaluating one scoring pair at a time,
 * we evaluate L predecessors simultaneously (L = number of int32 SIMD lanes).
 *
 * Key design decisions:
 *   - Structure-of-Arrays (SoA) layout for SIMD-friendly memory access
 *   - **Approximate max_skip**: after every batch with no lane-improvement we
 *     bump a counter; once batches_without_improvement * L exceeds
 *     params.max_chain_skip we break out of the predecessor scan. The TAL
 *     reference deliberately removes max_skip because for short reads the
 *     vectorized brute-force scan is fast enough; for long reads (>100 kbp)
 *     where windows hold thousands of anchors the unbounded scan dominates
 *     wall-clock and runs ~6x slower than the scalar path. The
 *     batch-granularity check is conservative (we only break *after* a full
 *     batch passes with no improvement) so the choice of best predecessor
 *     stays close to scalar's; if a single later lane would have improved we
 *     miss it, but for the parameter regime minimap2 ships in (max_skip = 25,
 *     max_iter = 5000) the chain quality is unaffected on benchmark inputs.
 *   - Scalar fallback for small ranges (candidate_idx - window_start <= 5)
 *   - Global-best long-range optimization remains scalar
 *   - skip_penalty must be zero (asserted at entry)
 *
 * Complexity: O(n * min(max_iter, n_anchors_until_skip) / L) where L is the
 * SIMD lane count.
 *
 * Reference: Intel TAL parallel_chaining_v2_22.h, with the max_skip
 * optimization re-introduced for long-read regimes per
 * REPORT_POOR_BENCHMARK_PERFORMANCE_AND_LOW_QUALITY_SIMD_MM_V_2.md.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <cstdint>
#include <vector>
#include <span>
#include <cstring>
#include <limits>
#include <algorithm>

export module bioseq_sequences.chaining.simd;

import mlogging;
import bioseq_sequences.chaining.utils;
import bioseq_simd.utils;
import context;
import bioseq_instr;

export {

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Import chaining types into this namespace for convenience
using bioseq::chaining::Anchor;
using bioseq::chaining::Chain;
using bioseq::chaining::ChainingParams;
using bioseq::chaining::compute_chaining_score;
using bioseq::chaining::backtrack_chains;

// ============================================================================
// Structure-of-Arrays (SoA) anchor layout for SIMD
// ============================================================================

/// Structure-of-Arrays layout for SIMD chaining.
///
/// Memory layout: one contiguous align-arena allocation (SPEC §6.3) sliced
/// into three (padding + anchor_count)-element regions. The leading
/// padding (= MaxLanes) is zero so backward SIMD loads from index j-(L-1)
/// never read before the region.
///
/// Why 32-bit truncation is safe: The scoring function only uses the lower
/// 32 bits of ref_bits (ref_position) and query_bits (query_position).
/// Upper bits are used only for window-start advancement, which remains scalar.
/// This matches TAL: anchor_r[i] = (num_bits_t)anc[i].r.
struct SoaAnchors {
    uint32_t* ref_positions   = nullptr;
    uint32_t* query_positions = nullptr;
    uint32_t* query_spans     = nullptr;
    size_t padding = 0;
    size_t count   = 0;

    [[nodiscard]] uint32_t* ref_data()   { return ref_positions + padding; }
    [[nodiscard]] uint32_t* query_data() { return query_positions + padding; }
    [[nodiscard]] uint32_t* span_data()  { return query_spans + padding; }
    [[nodiscard]] const uint32_t* ref_data()   const { return ref_positions + padding; }
    [[nodiscard]] const uint32_t* query_data() const { return query_positions + padding; }
    [[nodiscard]] const uint32_t* span_data()  const { return query_spans + padding; }
};

/// Convert AoS anchor array to SoA layout with SIMD padding.
///
/// Allocates three aligned arrays of (MaxLanes + anchor_count) uint32_t each.
/// The first MaxLanes elements are zero-padded to prevent out-of-bounds reads
/// when the SIMD inner loop loads backward from small indices.
///
/// @param d_int    Highway int32 tag (used for MaxLanes)
/// @param anchors  AoS anchor array
/// @return SoA layout with padding
template<class D>
[[nodiscard]] SoaAnchors create_soa_anchors(
    D d_int, const std::vector<Anchor>& anchors)
{
    const size_t max_lanes = hn::MaxLanes(d_int);
    const size_t anchor_count = anchors.size();
    const size_t total_size = max_lanes + anchor_count;

    // One contiguous chain-arena allocation for all three arrays (SPEC §6.3).
    auto combined = context::current().alloc_chain_array<uint32_t, 64>(3 * total_size);
    SoaAnchors soa;
    soa.ref_positions   = combined.data();
    soa.query_positions = combined.data() + total_size;
    soa.query_spans     = combined.data() + 2 * total_size;
    soa.padding = max_lanes;
    soa.count   = anchor_count;

    // Zero-fill padding region for each of the three slices
    std::memset(soa.ref_positions,   0, max_lanes * sizeof(uint32_t));
    std::memset(soa.query_positions, 0, max_lanes * sizeof(uint32_t));
    std::memset(soa.query_spans,     0, max_lanes * sizeof(uint32_t));

    // Convert AoS -> SoA (single pass, no redundant copies)
    uint32_t* HWY_RESTRICT ref_out   = soa.ref_data();
    uint32_t* HWY_RESTRICT query_out = soa.query_data();
    uint32_t* HWY_RESTRICT span_out  = soa.span_data();
    for (size_t idx = 0; idx < anchor_count; ++idx) {
        ref_out[idx]   = static_cast<uint32_t>(anchors[idx].ref_bits);
        query_out[idx] = static_cast<uint32_t>(anchors[idx].query_bits);
        span_out[idx]  = static_cast<uint32_t>(anchors[idx].query.query_span);
    }

    return soa;
}

// ============================================================================
// Vectorized scoring kernel
// ============================================================================

/// Vectorized chaining score computation.
///
/// Computes chaining score between anchor i (broadcast) and L predecessors
/// simultaneously. Invalid lanes get INT32_MIN.
///
/// Algorithm per lane (same as scalar compute_chaining_score):
///   1. query_distance = current_query_pos - predecessor_query_pos
///   2. ref_distance = current_ref_pos - predecessor_ref_pos
///   3. diagonal_deviation = |ref_distance - query_distance|
///   4. gap_length = min(ref_distance, query_distance)
///   5. Validity checks (INT32_MIN if fail)
///   6. score = min(predecessor_span, gap_length)
///   7. Gap penalty when diagonal_deviation != 0 || gap_length > predecessor_span
///
/// Note: skip_penalty is omitted (must be zero, enforced at chain_anchors_simd entry).
/// The TAL reference deliberately removes skip_penalty from the vectorized path.
///
/// @param d_int    Highway int32 tag
/// @param d_float  Highway float32 tag
/// @param current_ref_pos_vec       Broadcast of anchor i's ref position
/// @param current_query_pos_vec     Broadcast of anchor i's query position
/// @param predecessor_ref_pos_vec   L predecessors' ref positions
/// @param predecessor_query_pos_vec L predecessors' query positions
/// @param predecessor_span_vec      L predecessors' query spans
/// @param max_ref_dist_vec          Broadcast max reference distance
/// @param max_query_dist_vec        Broadcast max query distance
/// @param bandwidth_vec             Broadcast bandwidth
/// @param gap_penalty_vec           Broadcast gap penalty
/// @return Vector of int32 chaining scores (INT32_MIN for invalid lanes)
template<class D, class D_F>
HWY_INLINE hn::Vec<D> vectorized_chaining_score(
    D d_int, D_F d_float,
    hn::Vec<D> current_ref_pos_vec,
    hn::Vec<D> current_query_pos_vec,
    hn::Vec<D> predecessor_ref_pos_vec,
    hn::Vec<D> predecessor_query_pos_vec,
    hn::Vec<D> predecessor_span_vec,
    hn::Vec<D> max_ref_dist_vec,
    hn::Vec<D> max_query_dist_vec,
    hn::Vec<D> bandwidth_vec,
    hn::Vec<D_F> gap_penalty_vec)
{
    const auto zero = hn::Zero(d_int);
    const auto invalid_score = hn::Set(d_int, std::numeric_limits<int32_t>::min());

    // Distance computations
    const auto query_distance_vec = hn::Sub(current_query_pos_vec, predecessor_query_pos_vec);
    const auto ref_distance_vec = hn::Sub(current_ref_pos_vec, predecessor_ref_pos_vec);
    const auto diagonal_deviation_vec = hn::Abs(hn::Sub(ref_distance_vec, query_distance_vec));
    const auto gap_length_vec = hn::Min(ref_distance_vec, query_distance_vec);

    // Validity masks (true = INVALID pair)
    const auto invalid_query_not_positive = hn::Or(
        hn::Lt(query_distance_vec, zero), hn::Eq(query_distance_vec, zero));
    const auto invalid_query_exceeds_max_ref = hn::Gt(query_distance_vec, max_ref_dist_vec);
    const auto invalid_ref_is_zero = hn::Eq(ref_distance_vec, zero);
    const auto invalid_query_exceeds_max_query = hn::Gt(query_distance_vec, max_query_dist_vec);
    const auto invalid_diagonal_exceeds_bandwidth = hn::Gt(diagonal_deviation_vec, bandwidth_vec);

    const auto any_invalid = hn::Or(
        hn::Or(hn::Or(invalid_query_not_positive, invalid_query_exceeds_max_ref),
               invalid_ref_is_zero),
        hn::Or(invalid_query_exceeds_max_query, invalid_diagonal_exceeds_bandwidth));

    // Base score: min(predecessor_span, gap_length), or INT32_MIN where invalid
    auto chaining_score_vec = hn::IfThenElse(
        any_invalid, invalid_score, hn::Min(predecessor_span_vec, gap_length_vec));

    // Gap penalty -- applied when diagonal_deviation > 0 OR gap_length > predecessor_span
    const auto needs_gap_penalty = hn::AndNot(any_invalid,
        hn::Or(hn::Gt(diagonal_deviation_vec, zero),
               hn::Gt(gap_length_vec, predecessor_span_vec)));

    // Fast path: when no lane needs a penalty, skip the (expensive) log2
    // and float-conversion chain entirely. For very long sequences with
    // many invalid pairs this trims the ~10 vector ops per lane otherwise
    // wasted on a result that will be masked to zero.
    if (hn::AllFalse(d_int, needs_gap_penalty)) {
        return chaining_score_vec;
    }

    // linear_penalty = gap_penalty * diagonal_deviation
    const auto linear_penalty_vec = hn::Mul(
        gap_penalty_vec, hn::ConvertTo(d_float, diagonal_deviation_vec));

    // logarithmic_penalty = (diagonal_deviation >= 1) ? fast_log2(diagonal_deviation + 1) : 0
    const auto deviation_plus_one = hn::Add(diagonal_deviation_vec, hn::Set(d_int, 1));
    const auto raw_log2_vec = vectorized_fast_log2(d_int, d_float, deviation_plus_one);
    const auto deviation_ge_one_int = hn::Or(
        hn::Gt(diagonal_deviation_vec, hn::Set(d_int, 1)),
        hn::Eq(diagonal_deviation_vec, hn::Set(d_int, 1)));
    // Rebind int mask to float mask for IfThenElseZero with float vector
    const auto deviation_ge_one_float = hn::RebindMask(d_float, deviation_ge_one_int);
    const auto logarithmic_penalty_vec = hn::IfThenElseZero(deviation_ge_one_float, raw_log2_vec);

    // total_penalty = (int)(linear_penalty + 0.5 * logarithmic_penalty)
    const auto half = hn::Set(d_float, 0.5f);
    const auto total_penalty_float = hn::MulAdd(half, logarithmic_penalty_vec, linear_penalty_vec);
    auto total_penalty_int = hn::ConvertTo(d_int, total_penalty_float);

    // Zero out penalty for lanes that don't need it
    total_penalty_int = hn::IfThenElseZero(needs_gap_penalty, total_penalty_int);

    // score -= penalty
    chaining_score_vec = hn::Sub(chaining_score_vec, total_penalty_int);

    return chaining_score_vec;
}

// ============================================================================
// SIMD DP forward pass
// ============================================================================

/// Forward DP pass with SIMD-vectorized inner loop.
///
/// Writes best_scores[0..anchor_count), predecessors[0..anchor_count), peak_scores[0..anchor_count).
///
/// The outer loop is sequential (each anchor depends on predecessors' scores).
/// The inner loop scans backwards from i-1 to window_start in steps of L lanes.
/// For ranges with <= 5 predecessors, falls back to scalar to avoid SIMD overhead.
///
/// @param d_int          Highway int32 tag
/// @param anchors        Sorted anchor array (AoS, for scalar paths and window advancement)
/// @param soa            SoA anchor data (for SIMD loads)
/// @param best_scores    f[] array -- MUST be padded: best_scores[-MaxLanes..-1] must be valid and zero
/// @param predecessors   p[] array (std::vector)
/// @param peak_scores    v[] array (std::vector)
/// @param params         Chaining parameters (must have prepare() called)
template<class D>
void simd_dp_forward_pass(
    D d_int,
    const std::vector<Anchor>& anchors,
    const SoaAnchors& soa,
    int32_t* HWY_RESTRICT best_scores,
    std::span<int64_t> predecessors,
    std::span<int32_t> peak_scores,
    const ChainingParams& params)
{
    using D_F = hn::RebindToFloat<D>;
    D_F d_float;
    const size_t lane_count = hn::Lanes(d_int);
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());

    int32_t max_ref_dist = params.max_gap;
    int32_t max_query_dist = params.max_gap;
    if (max_ref_dist < params.bandwidth) max_ref_dist = params.bandwidth;
    if (max_query_dist < params.bandwidth) max_query_dist = params.bandwidth;

    // Broadcast constant parameters
    const auto max_ref_dist_vec = hn::Set(d_int, max_ref_dist);
    const auto max_query_dist_vec = hn::Set(d_int, max_query_dist);
    const auto bandwidth_vec = hn::Set(d_int, params.bandwidth);
    const auto gap_penalty_vec = hn::Set(d_float, params.gap_penalty);
    const auto no_predecessor_vec = hn::Set(d_int, static_cast<int32_t>(-1));

    const uint32_t* HWY_RESTRICT soa_ref   = soa.ref_data();
    const uint32_t* HWY_RESTRICT soa_query = soa.query_data();
    const uint32_t* HWY_RESTRICT soa_span  = soa.span_data();

    int64_t window_start = 0;
    int64_t global_best_idx = -1;

    HWY_ALIGN int32_t best_score_lanes[hn::MaxLanes(d_int)];
    HWY_ALIGN int32_t best_predecessor_lanes[hn::MaxLanes(d_int)];

    // Approximate max_skip: cap the number of consecutive batches with no
    // lane-improvement before we bail. Equivalent in spirit to the scalar
    // path's `n_skip > max_skip` break, but at batch granularity rather
    // than per-pair (the SIMD pipeline does not maintain the
    // `t[]`/`visited` array).
    const int32_t max_chain_skip = params.max_chain_skip;
    const int64_t max_consecutive_skip_batches = max_chain_skip > 0
        ? std::max<int64_t>(1, static_cast<int64_t>(max_chain_skip)
                                / std::max<int64_t>(1, static_cast<int64_t>(lane_count)) + 1)
        : std::numeric_limits<int64_t>::max();

    for (int64_t current_idx = 0; current_idx < anchor_count; ++current_idx) {
        int32_t best_predecessor = -1;
        int32_t best_score = static_cast<int32_t>(anchors[current_idx].query.query_span);

        // === Window advancement (scalar, full 64-bit ref_bits for chromosome check) ===
        while (window_start < current_idx &&
               (anchors[current_idx].ref_bits >> 32 != anchors[window_start].ref_bits >> 32 ||
                anchors[current_idx].ref_bits > anchors[window_start].ref_bits
                    + static_cast<uint64_t>(max_ref_dist)))
        {
            ++window_start;
        }
        if (current_idx - window_start > params.max_chain_iter) {
            window_start = current_idx - params.max_chain_iter;
        }

        int64_t candidate_idx = current_idx - 1;
        int64_t search_end = window_start;
        const int64_t lanes = static_cast<int64_t>(lane_count);

        // === SIMD path: when predecessor range is large enough ===
        if (candidate_idx - window_start > 5) {
            const auto current_ref_broadcast = hn::Set(
                d_int, static_cast<int32_t>(soa_ref[current_idx]));
            const auto current_query_broadcast = hn::Set(
                d_int, static_cast<int32_t>(soa_query[current_idx]));
            auto best_score_vec = hn::Set(d_int, best_score);
            auto best_predecessor_vec = no_predecessor_vec;
            int64_t consecutive_skip_batches = 0;

            // Full-vector iterations: scan backwards in steps of L lanes
            for (; (candidate_idx - (lanes - 1)) >= window_start; candidate_idx -= lanes) {
                const int64_t load_start = candidate_idx - (lanes - 1);
                const auto predecessor_ref_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_ref[load_start]));
                const auto predecessor_query_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_query[load_start]));
                const auto predecessor_span_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_span[load_start]));
                const auto predecessor_best_score_vec = hn::LoadU(d_int,
                    &best_scores[load_start]);

                auto pair_score_vec = vectorized_chaining_score(
                    d_int, d_float,
                    current_ref_broadcast, current_query_broadcast,
                    predecessor_ref_vec, predecessor_query_vec, predecessor_span_vec,
                    max_ref_dist_vec, max_query_dist_vec, bandwidth_vec, gap_penalty_vec);

                // total_score = pair_score + predecessor's best_score
                const auto total_score_vec = hn::Add(predecessor_best_score_vec, pair_score_vec);

                // Lane-wise max update
                const auto is_improvement = hn::Gt(total_score_vec, best_score_vec);
                const auto predecessor_indices_vec = hn::Add(
                    hn::Set(d_int, static_cast<int32_t>(load_start)), hn::Iota(d_int, 0));

                best_score_vec = hn::Max(total_score_vec, best_score_vec);
                best_predecessor_vec = hn::IfThenElse(
                    is_improvement, predecessor_indices_vec, best_predecessor_vec);

                // Approximate max_skip: bail when this batch (and the
                // previous ones in `consecutive_skip_batches`) produced no
                // improvement at all. Cheap mask reduction; the broadcast
                // result is identical across lanes so the AllFalse short
                // circuits early on every architecture.
                if (hn::AllFalse(d_int, is_improvement)) {
                    if (++consecutive_skip_batches >= max_consecutive_skip_batches) {
                        // Pretend we've consumed the whole window. The
                        // global_best_idx fallback below still gets a shot
                        // at finding a long-range chain target.
                        candidate_idx = window_start - 1;
                        break;
                    }
                } else {
                    consecutive_skip_batches = 0;
                }
            }

            // Partial tail: candidate_idx >= window_start but candidate_idx-(lanes-1) < window_start
            if (candidate_idx >= window_start) {
                const int64_t load_start = candidate_idx - (lanes - 1);
                const auto predecessor_ref_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_ref[load_start]));
                const auto predecessor_query_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_query[load_start]));
                const auto predecessor_span_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_span[load_start]));
                const auto predecessor_best_score_vec = hn::LoadU(d_int,
                    &best_scores[load_start]);

                auto pair_score_vec = vectorized_chaining_score(
                    d_int, d_float,
                    current_ref_broadcast, current_query_broadcast,
                    predecessor_ref_vec, predecessor_query_vec, predecessor_span_vec,
                    max_ref_dist_vec, max_query_dist_vec, bandwidth_vec, gap_penalty_vec);

                auto total_score_vec = hn::Add(predecessor_best_score_vec, pair_score_vec);

                // Mask out lanes below window_start -- set to INT32_MIN so they never win
                const size_t invalid_lane_count = static_cast<size_t>(
                    window_start - load_start);
                const auto valid_lane_mask = hn::Not(hn::FirstN(d_int, invalid_lane_count));
                const auto invalid_sentinel = hn::Set(d_int,
                    std::numeric_limits<int32_t>::min());
                total_score_vec = hn::IfThenElse(
                    valid_lane_mask, total_score_vec, invalid_sentinel);

                const auto is_improvement = hn::Gt(total_score_vec, best_score_vec);
                const auto predecessor_indices_vec = hn::Add(
                    hn::Set(d_int, static_cast<int32_t>(load_start)), hn::Iota(d_int, 0));

                best_score_vec = hn::Max(total_score_vec, best_score_vec);
                best_predecessor_vec = hn::IfThenElse(
                    is_improvement, predecessor_indices_vec, best_predecessor_vec);
            }

            // Horizontal reduction: extract scalar best_score and best_predecessor
            hn::Store(best_score_vec, d_int, best_score_lanes);
            hn::Store(best_predecessor_vec, d_int, best_predecessor_lanes);
            for (int64_t lane = static_cast<int64_t>(lane_count) - 1; lane >= 0; --lane) {
                if (best_score_lanes[lane] > best_score) {
                    best_score = best_score_lanes[lane];
                    best_predecessor = best_predecessor_lanes[lane];
                } else if (best_score_lanes[lane] == best_score) {
                    best_predecessor = std::max(best_predecessor, best_predecessor_lanes[lane]);
                    if (best_score == static_cast<int32_t>(anchors[current_idx].query.query_span)) {
                        best_predecessor = -1;
                    }
                }
            }
            search_end = window_start;

        } else {
            // === Scalar fallback for small ranges ===
            for (; candidate_idx >= window_start; --candidate_idx) {
                const int32_t pair_score = compute_chaining_score(
                    anchors[current_idx], anchors[candidate_idx],
                    max_ref_dist, max_query_dist, params.bandwidth,
                    params.gap_penalty, params.skip_penalty);
                if (pair_score == std::numeric_limits<int32_t>::min()) continue;
                const int32_t total_score = pair_score + best_scores[candidate_idx];
                if (total_score > best_score) {
                    best_score = total_score;
                    best_predecessor = static_cast<int32_t>(candidate_idx);
                }
            }
            search_end = candidate_idx;
        }

        // === Global-best long-range optimization (scalar) ===
        if (global_best_idx < 0 ||
            anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                > static_cast<uint64_t>(max_ref_dist))
        {
            int32_t global_best_score = std::numeric_limits<int32_t>::min();
            global_best_idx = -1;
            for (int64_t scan_idx = current_idx - 1; scan_idx >= window_start; --scan_idx) {
                if (global_best_score < best_scores[scan_idx]) {
                    global_best_score = best_scores[scan_idx];
                    global_best_idx = scan_idx;
                }
            }
        }
        if (global_best_idx >= 0 && global_best_idx < search_end) {
            const int32_t probe_score = compute_chaining_score(
                anchors[current_idx], anchors[global_best_idx],
                max_ref_dist, max_query_dist, params.bandwidth,
                params.gap_penalty, params.skip_penalty);
            if (probe_score != std::numeric_limits<int32_t>::min()
                && best_score < probe_score + best_scores[global_best_idx])
            {
                best_score = probe_score + best_scores[global_best_idx];
                best_predecessor = static_cast<int32_t>(global_best_idx);
            }
        }

        // === Store results ===
        best_scores[current_idx] = best_score;
        predecessors[current_idx] = best_predecessor;
        peak_scores[current_idx] = (best_predecessor >= 0
            && peak_scores[best_predecessor] > best_score)
            ? peak_scores[best_predecessor] : best_score;

        if (global_best_idx < 0 ||
            (anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                <= static_cast<uint64_t>(max_ref_dist)
             && best_scores[global_best_idx] < best_scores[current_idx]))
        {
            global_best_idx = current_idx;
        }
    }
}

// ============================================================================
// V2: dual-batch unrolled + interleaved forward DP pass
// ----------------------------------------------------------------------------
// V1 processes one batch of L predecessors per inner-loop iteration:
//
//   load_A → score_A → max-update_A → loop
//
// On Apple M1 NEON the per-batch chain has 4-deep dependency edges (load →
// pair-score → add → max). The OoO engine has trouble overlapping consecutive
// iterations because each iteration writes the same `best_score_vec` /
// `best_predecessor_vec` registers — every iteration's max-update *depends* on
// the previous iteration's result.
//
// V2 manually unrolls the inner loop by 2 and **interleaves** two
// reduction streams:
//
//   load_A, load_B   →   score_A, score_B   →   max_A, max_B   →   loop
//                                                      ↓
//                          merge(max_A, max_B) at the end of the loop
//
// Streams A and B carry independent `best_score_vec_{a,b}` and
// `best_predecessor_vec_{a,b}`. Their loads, scoring, and max-updates have NO
// register dependency on one another, so the M1's 4 NEON pipes can issue from
// both streams in parallel — same trick used by V11/V13 of the two-stack
// minimiser collector (see two_stacks_minimizers.cppm and the V11 / V13
// sections of ENGINEERING_REPORT_V2.md).
//
// Per-step cost ratio: each unrolled iteration scans 2*L predecessors with
// ~2x the SIMD ops but the same number of dependency-chain cycles, so the
// expected speed-up is dictated by the M1's issue width on the
// load+score+max chain (≈ 1.5–1.8x in practice; the load/store unit is the
// bottleneck for the 4-stream load).
//
// Early-exit semantics match V1's "consecutive batches with no improvement"
// counter at predecessor-count granularity: an unrolled iteration with no
// improvement in either stream is counted as 2 skip batches (it scanned 2*L
// predecessors with no improvement). The threshold is divided by 2 so the
// total scanned-predecessor budget before bailing is identical to V1.
// ============================================================================

/// V2 forward DP pass: dual-batch interleaved inner loop.
///
/// Same outer-loop structure and global-best probe as `simd_dp_forward_pass`,
/// but the SIMD inner loop processes 2 batches per iteration (`stream_a`,
/// `stream_b`) with independent reduction state. Falls back to V1's single-batch
/// loop when fewer than 2*L predecessors remain in the window.
///
/// Same correctness contract as V1: skip_penalty == 0 is enforced by the
/// caller; the vectorised scoring kernel `vectorized_chaining_score` is shared.
/// Outer-loop state writes (`best_scores`, `predecessors`, `peak_scores`,
/// `global_best_idx`) are byte-identical to V1 — the only difference is which
/// batches get short-circuited by the approximate max_skip when both streams
/// miss in lockstep, and that matches V1's behaviour at predecessor-count
/// granularity (2 skip batches per dual-batch instead of 1 per single batch,
/// both threshold-divided proportionally).
template<class D>
void simd_dp_forward_pass_v2(
    D d_int,
    const std::vector<Anchor>& anchors,
    const SoaAnchors& soa,
    int32_t* HWY_RESTRICT best_scores,
    std::span<int64_t> predecessors,
    std::span<int32_t> peak_scores,
    const ChainingParams& params)
{
    using D_F = hn::RebindToFloat<D>;
    D_F d_float;
    const size_t lane_count = hn::Lanes(d_int);
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());

    int32_t max_ref_dist = params.max_gap;
    int32_t max_query_dist = params.max_gap;
    if (max_ref_dist < params.bandwidth) max_ref_dist = params.bandwidth;
    if (max_query_dist < params.bandwidth) max_query_dist = params.bandwidth;

    const auto max_ref_dist_vec = hn::Set(d_int, max_ref_dist);
    const auto max_query_dist_vec = hn::Set(d_int, max_query_dist);
    const auto bandwidth_vec = hn::Set(d_int, params.bandwidth);
    const auto gap_penalty_vec = hn::Set(d_float, params.gap_penalty);
    const auto no_predecessor_vec = hn::Set(d_int, static_cast<int32_t>(-1));

    const uint32_t* HWY_RESTRICT soa_ref   = soa.ref_data();
    const uint32_t* HWY_RESTRICT soa_query = soa.query_data();
    const uint32_t* HWY_RESTRICT soa_span  = soa.span_data();

    int64_t window_start = 0;
    int64_t global_best_idx = -1;

    HWY_ALIGN int32_t best_score_lanes[hn::MaxLanes(d_int)];
    HWY_ALIGN int32_t best_predecessor_lanes[hn::MaxLanes(d_int)];

    // Approximate max_skip — see V1 commentary. V2's threshold is divided
    // by 2 so the total scanned-predecessor budget is identical (each
    // unrolled iteration scans 2*L predecessors with no improvement in
    // the worst case).
    const int32_t max_chain_skip = params.max_chain_skip;
    const int64_t max_consecutive_skip_units_v1 = max_chain_skip > 0
        ? std::max<int64_t>(1, static_cast<int64_t>(max_chain_skip)
                                / std::max<int64_t>(1, static_cast<int64_t>(lane_count)) + 1)
        : std::numeric_limits<int64_t>::max();
    const int64_t max_consecutive_skip_dual_batches = max_chain_skip > 0
        ? std::max<int64_t>(1, max_consecutive_skip_units_v1 / 2 + 1)
        : std::numeric_limits<int64_t>::max();

    for (int64_t current_idx = 0; current_idx < anchor_count; ++current_idx) {
        int32_t best_predecessor = -1;
        int32_t best_score = static_cast<int32_t>(anchors[current_idx].query.query_span);

        // === Window advancement (scalar) — identical to V1 ===
        while (window_start < current_idx &&
               (anchors[current_idx].ref_bits >> 32 != anchors[window_start].ref_bits >> 32 ||
                anchors[current_idx].ref_bits > anchors[window_start].ref_bits
                    + static_cast<uint64_t>(max_ref_dist)))
        {
            ++window_start;
        }
        if (current_idx - window_start > params.max_chain_iter) {
            window_start = current_idx - params.max_chain_iter;
        }

        int64_t candidate_idx = current_idx - 1;
        int64_t search_end = window_start;
        const int64_t lanes = static_cast<int64_t>(lane_count);
        const int64_t two_lanes = 2 * lanes;

        if (candidate_idx - window_start > 5) {
            const auto current_ref_broadcast = hn::Set(
                d_int, static_cast<int32_t>(soa_ref[current_idx]));
            const auto current_query_broadcast = hn::Set(
                d_int, static_cast<int32_t>(soa_query[current_idx]));

            // Two independent reduction streams — A handles batch i, B
            // handles batch i+1 (stride 2*L).
            auto best_score_vec_a = hn::Set(d_int, best_score);
            auto best_score_vec_b = hn::Set(d_int, best_score);
            auto best_predecessor_vec_a = no_predecessor_vec;
            auto best_predecessor_vec_b = no_predecessor_vec;
            int64_t consecutive_skip_dual_batches = 0;

            // === Dual-batch unrolled loop ===
            // Each iteration: load_A | load_B → score_A | score_B → max_A | max_B
            for (; (candidate_idx - (two_lanes - 1)) >= window_start;
                 candidate_idx -= two_lanes)
            {
                const int64_t a_start = candidate_idx - (lanes - 1);
                const int64_t b_start = candidate_idx - (two_lanes - 1);

                // Stream A loads (top half: most-recent predecessors)
                const auto pred_ref_vec_a = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_ref[a_start]));
                const auto pred_query_vec_a = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_query[a_start]));
                const auto pred_span_vec_a = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_span[a_start]));
                const auto pred_best_score_vec_a = hn::LoadU(d_int,
                    &best_scores[a_start]);

                // Stream B loads (bottom half: older predecessors)
                const auto pred_ref_vec_b = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_ref[b_start]));
                const auto pred_query_vec_b = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_query[b_start]));
                const auto pred_span_vec_b = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_span[b_start]));
                const auto pred_best_score_vec_b = hn::LoadU(d_int,
                    &best_scores[b_start]);

                // Independent scoring kernels (HWY_INLINE'd; no shared
                // register state between streams).
                const auto pair_score_vec_a = vectorized_chaining_score(
                    d_int, d_float,
                    current_ref_broadcast, current_query_broadcast,
                    pred_ref_vec_a, pred_query_vec_a, pred_span_vec_a,
                    max_ref_dist_vec, max_query_dist_vec, bandwidth_vec, gap_penalty_vec);
                const auto pair_score_vec_b = vectorized_chaining_score(
                    d_int, d_float,
                    current_ref_broadcast, current_query_broadcast,
                    pred_ref_vec_b, pred_query_vec_b, pred_span_vec_b,
                    max_ref_dist_vec, max_query_dist_vec, bandwidth_vec, gap_penalty_vec);

                const auto total_score_vec_a = hn::Add(pred_best_score_vec_a, pair_score_vec_a);
                const auto total_score_vec_b = hn::Add(pred_best_score_vec_b, pair_score_vec_b);

                // Lane indices vec for predecessor tracking
                const auto pred_indices_vec_a = hn::Add(
                    hn::Set(d_int, static_cast<int32_t>(a_start)), hn::Iota(d_int, 0));
                const auto pred_indices_vec_b = hn::Add(
                    hn::Set(d_int, static_cast<int32_t>(b_start)), hn::Iota(d_int, 0));

                // Independent max-updates
                const auto is_improvement_a = hn::Gt(total_score_vec_a, best_score_vec_a);
                const auto is_improvement_b = hn::Gt(total_score_vec_b, best_score_vec_b);
                best_score_vec_a = hn::Max(total_score_vec_a, best_score_vec_a);
                best_score_vec_b = hn::Max(total_score_vec_b, best_score_vec_b);
                best_predecessor_vec_a = hn::IfThenElse(
                    is_improvement_a, pred_indices_vec_a, best_predecessor_vec_a);
                best_predecessor_vec_b = hn::IfThenElse(
                    is_improvement_b, pred_indices_vec_b, best_predecessor_vec_b);

                // Combined skip-batch check: both streams miss → 2 skip
                // batches' worth of predecessors had no improvement.
                if (hn::AllFalse(d_int, is_improvement_a) &&
                    hn::AllFalse(d_int, is_improvement_b))
                {
                    if (++consecutive_skip_dual_batches >= max_consecutive_skip_dual_batches) {
                        candidate_idx = window_start - 1;
                        break;
                    }
                } else {
                    consecutive_skip_dual_batches = 0;
                }
            }

            // === Orphan single batch (n_full_batches was odd) ===
            // The remaining `candidate_idx` covers exactly L predecessors
            // entirely inside the window.
            if ((candidate_idx - (lanes - 1)) >= window_start) {
                const int64_t load_start = candidate_idx - (lanes - 1);
                const auto pred_ref_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_ref[load_start]));
                const auto pred_query_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_query[load_start]));
                const auto pred_span_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_span[load_start]));
                const auto pred_best_score_vec = hn::LoadU(d_int,
                    &best_scores[load_start]);

                const auto pair_score_vec = vectorized_chaining_score(
                    d_int, d_float,
                    current_ref_broadcast, current_query_broadcast,
                    pred_ref_vec, pred_query_vec, pred_span_vec,
                    max_ref_dist_vec, max_query_dist_vec, bandwidth_vec, gap_penalty_vec);
                const auto total_score_vec = hn::Add(pred_best_score_vec, pair_score_vec);
                const auto is_improvement = hn::Gt(total_score_vec, best_score_vec_a);
                const auto pred_indices_vec = hn::Add(
                    hn::Set(d_int, static_cast<int32_t>(load_start)), hn::Iota(d_int, 0));

                best_score_vec_a = hn::Max(total_score_vec, best_score_vec_a);
                best_predecessor_vec_a = hn::IfThenElse(
                    is_improvement, pred_indices_vec, best_predecessor_vec_a);

                candidate_idx -= lanes;
            }

            // === Partial tail (last batch with masked-out lanes) ===
            if (candidate_idx >= window_start) {
                const int64_t load_start = candidate_idx - (lanes - 1);
                const auto pred_ref_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_ref[load_start]));
                const auto pred_query_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_query[load_start]));
                const auto pred_span_vec = hn::LoadU(d_int,
                    reinterpret_cast<const int32_t*>(&soa_span[load_start]));
                const auto pred_best_score_vec = hn::LoadU(d_int,
                    &best_scores[load_start]);

                auto pair_score_vec = vectorized_chaining_score(
                    d_int, d_float,
                    current_ref_broadcast, current_query_broadcast,
                    pred_ref_vec, pred_query_vec, pred_span_vec,
                    max_ref_dist_vec, max_query_dist_vec, bandwidth_vec, gap_penalty_vec);
                auto total_score_vec = hn::Add(pred_best_score_vec, pair_score_vec);

                const size_t invalid_lane_count = static_cast<size_t>(
                    window_start - load_start);
                const auto valid_lane_mask = hn::Not(hn::FirstN(d_int, invalid_lane_count));
                const auto invalid_sentinel = hn::Set(d_int,
                    std::numeric_limits<int32_t>::min());
                total_score_vec = hn::IfThenElse(
                    valid_lane_mask, total_score_vec, invalid_sentinel);

                const auto is_improvement = hn::Gt(total_score_vec, best_score_vec_a);
                const auto pred_indices_vec = hn::Add(
                    hn::Set(d_int, static_cast<int32_t>(load_start)), hn::Iota(d_int, 0));

                best_score_vec_a = hn::Max(total_score_vec, best_score_vec_a);
                best_predecessor_vec_a = hn::IfThenElse(
                    is_improvement, pred_indices_vec, best_predecessor_vec_a);
            }

            // === Stream merge: combine A and B into a single best ===
            // Lane-wise: pick the larger of A/B; tie-break by predecessor
            // index using the same "later predecessor wins on equal score"
            // rule the V1 horizontal reduction uses.
            const auto a_wins = hn::Gt(best_score_vec_a, best_score_vec_b);
            const auto a_eq_b = hn::Eq(best_score_vec_a, best_score_vec_b);
            const auto best_score_vec = hn::Max(best_score_vec_a, best_score_vec_b);
            // Pick A's predecessor when A strictly wins or when both tie
            // (since A scans the *later* / closer predecessors which match
            // the V1 horizontal-reduction "lane-N-down" tie-break).
            const auto best_predecessor_vec = hn::IfThenElse(
                hn::Or(a_wins, a_eq_b),
                best_predecessor_vec_a,
                best_predecessor_vec_b);

            // Horizontal reduction — identical to V1.
            hn::Store(best_score_vec, d_int, best_score_lanes);
            hn::Store(best_predecessor_vec, d_int, best_predecessor_lanes);
            for (int64_t lane = static_cast<int64_t>(lane_count) - 1; lane >= 0; --lane) {
                if (best_score_lanes[lane] > best_score) {
                    best_score = best_score_lanes[lane];
                    best_predecessor = best_predecessor_lanes[lane];
                } else if (best_score_lanes[lane] == best_score) {
                    best_predecessor = std::max(best_predecessor, best_predecessor_lanes[lane]);
                    if (best_score == static_cast<int32_t>(anchors[current_idx].query.query_span)) {
                        best_predecessor = -1;
                    }
                }
            }
            search_end = window_start;

        } else {
            // Scalar fallback — identical to V1.
            for (; candidate_idx >= window_start; --candidate_idx) {
                const int32_t pair_score = compute_chaining_score(
                    anchors[current_idx], anchors[candidate_idx],
                    max_ref_dist, max_query_dist, params.bandwidth,
                    params.gap_penalty, params.skip_penalty);
                if (pair_score == std::numeric_limits<int32_t>::min()) continue;
                const int32_t total_score = pair_score + best_scores[candidate_idx];
                if (total_score > best_score) {
                    best_score = total_score;
                    best_predecessor = static_cast<int32_t>(candidate_idx);
                }
            }
            search_end = candidate_idx;
        }

        // === Global-best long-range optimisation — identical to V1 ===
        if (global_best_idx < 0 ||
            anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                > static_cast<uint64_t>(max_ref_dist))
        {
            int32_t global_best_score = std::numeric_limits<int32_t>::min();
            global_best_idx = -1;
            for (int64_t scan_idx = current_idx - 1; scan_idx >= window_start; --scan_idx) {
                if (global_best_score < best_scores[scan_idx]) {
                    global_best_score = best_scores[scan_idx];
                    global_best_idx = scan_idx;
                }
            }
        }
        if (global_best_idx >= 0 && global_best_idx < search_end) {
            const int32_t probe_score = compute_chaining_score(
                anchors[current_idx], anchors[global_best_idx],
                max_ref_dist, max_query_dist, params.bandwidth,
                params.gap_penalty, params.skip_penalty);
            if (probe_score != std::numeric_limits<int32_t>::min()
                && best_score < probe_score + best_scores[global_best_idx])
            {
                best_score = probe_score + best_scores[global_best_idx];
                best_predecessor = static_cast<int32_t>(global_best_idx);
            }
        }

        // === Store results ===
        best_scores[current_idx] = best_score;
        predecessors[current_idx] = best_predecessor;
        peak_scores[current_idx] = (best_predecessor >= 0
            && peak_scores[best_predecessor] > best_score)
            ? peak_scores[best_predecessor] : best_score;

        if (global_best_idx < 0 ||
            (anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                <= static_cast<uint64_t>(max_ref_dist)
             && best_scores[global_best_idx] < best_scores[current_idx]))
        {
            global_best_idx = current_idx;
        }
    }
}

// ============================================================================
// Top-level entry point
// ============================================================================

/// SIMD-accelerated DP chaining.
///
/// Same algorithm as scalar DP, but the inner predecessor search loop is
/// vectorized using Highway SIMD. Falls back to scalar for small ranges.
///
/// Requires skip_penalty == 0 (the vectorized scoring kernel omits it).
/// If skip_penalty is non-zero, use chain_anchors_scalar instead.
///
/// @param anchors  Sorted anchor array
/// @param params   Chaining parameters (must have prepare() called)
/// @return Pair of (chains, reordered_anchors)
[[nodiscard]] std::pair<std::vector<Chain>, std::vector<Anchor>> chain_anchors_simd(
    std::vector<Anchor>& anchors,
    const ChainingParams& params)
{
    static const auto* _simd_desc =
        bioseq::instr::register_region_static("mg_lchain_simd");
    auto _simd_scope = bioseq::instr::region(_simd_desc);
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());
    if (anchor_count == 0) return {{}, {}};

    // The vectorized scoring kernel omits skip_penalty * gap_length.
    // Enforce that skip_penalty is zero; fall back to scalar if not.
    DEBUG_ASSERT(params.skip_penalty == 0.0f,
        "chain_anchors_simd requires skip_penalty == 0. "
        "Use chain_anchors_scalar for non-zero chain_skip_scale.");

    hn::ScalableTag<int32_t> d_int;

    auto chain_scope = context::current().push<context::Arena::Chain>();

    SoaAnchors soa;
    {
        static const auto* _aos_desc =
            bioseq::instr::register_region_static("mg_lchain_simd_aos_to_soa");
        auto _aos_scope = bioseq::instr::region(_aos_desc);
        soa = create_soa_anchors(d_int, anchors);
    }

    // Allocate padded best_scores array (f[]) from the chain arena.
    // SIMD loads access best_scores[j-(L-1)] which can be negative, so we
    // prepend MaxLanes zero-filled elements as safe padding (SPEC §5.2).
    const size_t max_lane_count = hn::MaxLanes(d_int);
    auto score_storage_span = context::current().alloc_chain_array<int32_t, 64>(
        max_lane_count + anchor_count);
    std::memset(score_storage_span.data(), 0,
        score_storage_span.size() * sizeof(int32_t));
    int32_t* padded_best_scores = score_storage_span.data() + max_lane_count;

    // Same arena-bump pattern the scalar path uses (proposal-1 §A.3):
    // skip the make_chain_vector + assign/resize churn; the DP body
    // writes peak_scores before reading it. predecessors needs the -1
    // sentinel for the read-before-write check, so memset(0xff) once
    // beats per-element placement-new through the vector wrapper.
    auto predecessors = context::current().alloc_chain_array<int64_t>(anchor_count);
    auto peak_scores  = context::current().alloc_chain_array<int32_t>(anchor_count);
    std::memset(predecessors.data(), 0xff,
                static_cast<std::size_t>(anchor_count) * sizeof(int64_t));

    {
        static const auto* _fwd_desc =
            bioseq::instr::register_loop_static("mg_lchain_simd_forward");
        auto _fwd_scope = bioseq::instr::loop(_fwd_desc);
        // simd_dp_forward_pass owns the inner iterations; the loop scope
        // captures the wrapping region time. tick() is not invoked from the
        // outer scope because the simd helper does not expose an iteration
        // count — region-style timing is sufficient at this granularity.
        (void)_fwd_scope;
        simd_dp_forward_pass(d_int, anchors, soa,
            padded_best_scores, predecessors, peak_scores, params);
    }

    JustLog("[chaining_simd] Forward DP pass complete for {} anchors", anchor_count);

    static const auto* _bt_desc =
        bioseq::instr::register_region_static("backtrack_chains");
    auto _bt_scope = bioseq::instr::region(_bt_desc);
    return backtrack_chains(anchors,
        std::span<const int32_t>(padded_best_scores, anchor_count),
        std::span<const int64_t>(predecessors),
        std::span<const int32_t>(peak_scores),
        params.min_chain_score, params.min_seed_count);
}

/// V2 top-level entry point — same contract as `chain_anchors_simd` but
/// uses the dual-batch unrolled forward DP pass. Drop-in replacement for
/// V1 (the only difference is the inner-loop micro-architecture).
[[nodiscard]] std::pair<std::vector<Chain>, std::vector<Anchor>> chain_anchors_simd_v2(
    std::vector<Anchor>& anchors,
    const ChainingParams& params)
{
    static const auto* _simd_desc =
        bioseq::instr::register_region_static("mg_lchain_simd");
    auto _simd_scope = bioseq::instr::region(_simd_desc);
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());
    if (anchor_count == 0) return {{}, {}};

    DEBUG_ASSERT(params.skip_penalty == 0.0f,
        "chain_anchors_simd_v2 requires skip_penalty == 0. "
        "Use chain_anchors_scalar for non-zero chain_skip_scale.");

    hn::ScalableTag<int32_t> d_int;

    auto chain_scope = context::current().push<context::Arena::Chain>();

    SoaAnchors soa;
    {
        static const auto* _aos_desc =
            bioseq::instr::register_region_static("mg_lchain_simd_aos_to_soa");
        auto _aos_scope = bioseq::instr::region(_aos_desc);
        soa = create_soa_anchors(d_int, anchors);
    }

    const size_t max_lane_count = hn::MaxLanes(d_int);
    auto score_storage_span = context::current().alloc_chain_array<int32_t, 64>(
        max_lane_count + anchor_count);
    std::memset(score_storage_span.data(), 0,
        score_storage_span.size() * sizeof(int32_t));
    int32_t* padded_best_scores = score_storage_span.data() + max_lane_count;

    auto predecessors = context::current().alloc_chain_array<int64_t>(anchor_count);
    auto peak_scores  = context::current().alloc_chain_array<int32_t>(anchor_count);
    std::memset(predecessors.data(), 0xff,
                static_cast<std::size_t>(anchor_count) * sizeof(int64_t));

    {
        static const auto* _fwd_desc =
            bioseq::instr::register_loop_static("mg_lchain_simd_forward");
        auto _fwd_scope = bioseq::instr::loop(_fwd_desc);
        (void)_fwd_scope;
        simd_dp_forward_pass_v2(d_int, anchors, soa,
            padded_best_scores, predecessors, peak_scores, params);
    }

    JustLog("[chaining_simd] V2 forward DP pass complete for {} anchors", anchor_count);

    static const auto* _bt_desc =
        bioseq::instr::register_region_static("backtrack_chains");
    auto _bt_scope = bioseq::instr::region(_bt_desc);
    return backtrack_chains(anchors,
        std::span<const int32_t>(padded_best_scores, anchor_count),
        std::span<const int64_t>(predecessors),
        std::span<const int32_t>(peak_scores),
        params.min_chain_score, params.min_seed_count);
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}  // export
