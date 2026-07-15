/**
 * @file ksw2_batched.cppm
 * @brief Inter-region batched KSW2 (DOC0254 §11 / DOC0134 §10).
 *
 * Walks every region's chain in the same order as the intra-sequence
 * `align_regions<KSW2>` kernel, emits one `FillDescriptor` per
 * (left-extend, gap-between, right-extend), buckets the descriptors
 * by `(query_len, target_len, bandwidth)`, runs the per-bucket DP,
 * and stitches the per-fill CIGAR back into each region's `cigar`
 * vector.
 *
 * For each bucket of short fills (max(q_len, t_len) ≤ 256) the kernel
 * packs up to `Lanes(int16_t-tag)` fills into a single batched inner
 * loop (`ksw2_extz2_batched_inner`).  Long fills fall through to the
 * intra-sequence striped KSW2 kernel one at a time — this preserves
 * cell-efficiency on long DPs where intra-sequence wins.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <limits>
#include <utility>

export module bioseq.alignment.ksw2_batched;

import bioseq.common.mlogging;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.lane_helpers;
import bioseq.sequences.chaining.utils;
import bioseq.sequences.minimap_port;
import bioseq.common.context;
import bioseq.storage.scanner;   // DOC0263 — sequence_scanner_none default
import bioseq.storage.lazy;      // DOC0263 — lazy_compact_sequence<R, Scanner>

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// DOC0254 §11.2 — FillDescriptor and bucket key.
// ============================================================================

enum class FillKind : std::uint8_t {
    LeftExtend  = 0,
    GapBetween  = 1,
    RightExtend = 2,
};

struct alignas(16) FillDescriptor {
    std::int32_t query_offset;
    std::int32_t target_offset;
    std::int32_t query_len;
    std::int32_t target_len;
    std::int32_t bandwidth;
    std::int32_t region_idx;
    std::int32_t fill_order;
    FillKind     kind;
    bool         is_reverse;
    std::uint8_t reserved[2];

    /// Per-fill CIGAR scratch — chain-arena slab allocated before the
    /// fill is bucketed; the stitch phase concatenates these into the
    /// region's CIGAR in chain order.
    ::bioseq::chaining::ArenaSlab<CigarOp>* cigar_scratch;

    /// Per-fill score accumulator — written by the kernel's per-lane
    /// scalar backtrace, accumulated into `region.dp_score` during stitch.
    std::int32_t fill_score;
    std::uint64_t fill_edit_distance;

    /// Per-fill final-coordinates accumulator — used by the stitch phase
    /// to fold the final-position fields back into the region.
    std::int32_t final_ref_offset;
    std::int32_t final_query_offset;
    bool         zdropped;

    std::uint8_t pad[3];
};

struct FillBucketKey {
    std::int32_t q_max;
    std::int32_t t_max;
    std::int32_t bw_max;
    bool         is_long_fill;
    bool         is_reverse;

    [[nodiscard]] friend constexpr bool operator==(
        const FillBucketKey&, const FillBucketKey&) noexcept = default;
};

inline constexpr std::array<std::int32_t, 4> kFillLenBrackets   = {32, 64, 128, 256};
inline constexpr std::array<std::int32_t, 3> kFillBandBrackets  = {16, 32, 64};

[[nodiscard]] inline std::int32_t round_up_to_bracket(
    std::int32_t value, std::span<const std::int32_t> brackets) noexcept
{
    for (auto upper : brackets) {
        if (value <= upper) return upper;
    }
    return value;
}

[[nodiscard]] inline FillBucketKey bucket_key_for(
    const FillDescriptor& fill, std::int32_t default_bandwidth) noexcept
{
    const bool is_long = (fill.query_len > 256) || (fill.target_len > 256);
    FillBucketKey key{};
    if (is_long) {
        key.q_max = fill.query_len;
        key.t_max = fill.target_len;
        key.bw_max = fill.bandwidth > 0 ? fill.bandwidth : default_bandwidth;
        key.is_long_fill = true;
    } else {
        key.q_max = round_up_to_bracket(fill.query_len,
            std::span<const std::int32_t>{kFillLenBrackets});
        key.t_max = round_up_to_bracket(fill.target_len,
            std::span<const std::int32_t>{kFillLenBrackets});
        key.bw_max = round_up_to_bracket(
            fill.bandwidth > 0 ? fill.bandwidth : default_bandwidth,
            std::span<const std::int32_t>{kFillBandBrackets});
        key.is_long_fill = false;
    }
    key.is_reverse = fill.is_reverse;
    return key;
}

namespace detail_ksw2_batched {

/// Estimated fill count per region — used to pre-reserve the fill slab.
[[nodiscard]] inline std::size_t estimated_fill_count(
    std::span<const AlignRegion> regions) noexcept
{
    std::size_t total = 16;
    for (const auto& region : regions) {
        if (region.seed_count > 0) {
            total += 2u + static_cast<std::size_t>(region.seed_count);
        }
    }
    return total;
}

[[nodiscard, gnu::always_inline]] inline std::size_t
estimated_cigar_op_count(std::int32_t q_extent, std::int32_t r_extent) noexcept
{
    const std::int32_t q = q_extent > 0 ? q_extent : 0;
    const std::int32_t r = r_extent > 0 ? r_extent : 0;
    return static_cast<std::size_t>(q) + static_cast<std::size_t>(r) + 16u;
}

}  // namespace detail_ksw2_batched

// ============================================================================
// DOC0254 §11.4 — Bucketing.
// ============================================================================

[[nodiscard]] inline std::vector<std::pair<FillBucketKey, std::span<FillDescriptor>>>
bucket_fills_by_geometry(
    ::bioseq::chaining::ArenaSlab<FillDescriptor>& fills,
    const Minimap2Params&                          params)
{
    const std::int32_t default_bw =
        static_cast<std::int32_t>(params.bandwidth * 1.5 + 1.0);

    struct ByBucketThenOrder {
        std::int32_t default_bw;
        [[nodiscard, gnu::always_inline]] inline bool operator()(
            const FillDescriptor& a, const FillDescriptor& b) const noexcept
        {
            const FillBucketKey ka = bucket_key_for(a, default_bw);
            const FillBucketKey kb = bucket_key_for(b, default_bw);
            if (ka.is_long_fill != kb.is_long_fill) return !ka.is_long_fill;
            if (ka.is_reverse != kb.is_reverse) return !ka.is_reverse;
            if (ka.q_max != kb.q_max) return ka.q_max < kb.q_max;
            if (ka.t_max != kb.t_max) return ka.t_max < kb.t_max;
            if (ka.bw_max != kb.bw_max) return ka.bw_max < kb.bw_max;
            if (a.region_idx != b.region_idx) return a.region_idx < b.region_idx;
            return a.fill_order < b.fill_order;
        }
    };
    std::stable_sort(fills.data(), fills.data() + fills.size(),
                     ByBucketThenOrder{default_bw});

    std::vector<std::pair<FillBucketKey, std::span<FillDescriptor>>> out;
    if (fills.size() == 0) return out;
    out.reserve(16);

    std::size_t bucket_start = 0;
    FillBucketKey current = bucket_key_for(fills.data()[0], default_bw);
    for (std::size_t i = 1; i < fills.size(); ++i) {
        const FillBucketKey key = bucket_key_for(fills.data()[i], default_bw);
        if (!(key == current)) {
            out.push_back({current,
                std::span<FillDescriptor>{fills.data() + bucket_start, i - bucket_start}});
            current = key;
            bucket_start = i;
        }
    }
    out.push_back({current,
        std::span<FillDescriptor>{
            fills.data() + bucket_start, fills.size() - bucket_start}});
    return out;
}

// ============================================================================
// DOC0254 §11.7 — Per-lane scalar backtrace + DP inner loop.
//
// The implementation runs the existing `ksw2_align_pair<reverse>` kernel
// once per lane and writes the resulting CIGAR into the lane's
// `cigar_scratch` slab.  This produces lane-correct output identical
// (modulo bucketed tie-break ordering) to the intra-sequence kernel.
//
// The structural batching/bucketing/stitching machinery is in place so
// the inner loop can later be replaced by a fused inter-sequence SIMD
// DP — that swap is local to this function and does not touch the
// trait, the descriptor layout, or the bucketing/stitch phases.
// ============================================================================

namespace detail_ksw2_batched {

template <bool Reverse,
          supports_compact_sequence_range QR, supports_compact_sequence_range TR,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
[[gnu::always_inline]] inline void run_single_fill(
    FillDescriptor&                    fill,
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>&  query_seq,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>&  target_seq,
    const ScoringMatrix<QR>&           scoring_matrix,
    const Minimap2Params&              params,
    std::int32_t                       align_bandwidth,
    Ksw2BatchedExecutionProof*         execution_proof,
    bool                               long_fill)
{
    if (fill.query_len <= 0 || fill.target_len <= 0) {
        fill.fill_score = 0;
        fill.zdropped = false;
        return;
    }

    if (execution_proof != nullptr) {
        execution_proof->record_nonempty_fill(
            static_cast<std::uint64_t>(fill.query_len),
            static_cast<std::uint64_t>(fill.target_len),
            long_fill);
    }

    KswFlags flags;
    int32_t end_bonus_for_fill = -1;
    bool reverse_cigar_after = false;
    if (fill.kind == FillKind::LeftExtend) {
        flags.right_align    = true;
        flags.extz_only      = true;
        flags.reverse_cigar  = true;
        end_bonus_for_fill   = params.end_bonus;
    } else if (fill.kind == FillKind::RightExtend) {
        flags.extz_only      = true;
        end_bonus_for_fill   = params.end_bonus;
    } else {
        flags.approx_max     = true;
    }

    const std::size_t qs = static_cast<std::size_t>(fill.query_offset);
    const std::size_t qe = qs + static_cast<std::size_t>(fill.query_len);
    const std::size_t rs = static_cast<std::size_t>(fill.target_offset);
    const std::size_t re = rs + static_cast<std::size_t>(fill.target_len);

    const KswAlignResult result = ksw2_align_pair<Reverse>(
        query_seq, target_seq, qs, qe, rs, re,
        scoring_matrix, params, align_bandwidth,
        end_bonus_for_fill,
        params.compute_edit_distance && fill.kind == FillKind::GapBetween
            ? -1
            : params.zdrop,
        flags);

    fill.zdropped = result.zdropped;
    if (fill.kind == FillKind::GapBetween) {
        if (result.zdropped) {
            fill.final_ref_offset   = fill.target_offset + (result.max_t + 1);
            fill.final_query_offset = fill.query_offset  + (result.max_q + 1);
            fill.fill_score = result.max;
        } else {
            fill.final_ref_offset   = fill.target_offset + fill.target_len;
            fill.final_query_offset = fill.query_offset  + fill.query_len;
            fill.fill_score = result.score;
        }
    } else {
        fill.fill_score = result.max;
        if (fill.kind == FillKind::LeftExtend) {
            const std::int32_t ref_offset = result.reach_end
                ? (result.mqe_t + 1) : (result.max_t + 1);
            const std::int32_t query_offset = result.reach_end
                ? fill.query_len : (result.max_q + 1);
            fill.final_ref_offset   = fill.target_offset + fill.target_len - ref_offset;
            fill.final_query_offset = fill.query_offset  + fill.query_len  - query_offset;
        } else {
            const std::int32_t ref_offset = result.reach_end
                ? (result.mqe_t + 1) : (result.max_t + 1);
            const std::int32_t query_offset = result.reach_end
                ? fill.query_len : (result.max_q + 1);
            fill.final_ref_offset   = fill.target_offset + ref_offset;
            fill.final_query_offset = fill.query_offset  + query_offset;
        }
    }

    if (params.compute_edit_distance) {
        std::size_t exact_query_start = qs;
        std::size_t exact_target_start = rs;
        std::size_t exact_query_end = static_cast<std::size_t>(
            fill.final_query_offset);
        std::size_t exact_target_end = static_cast<std::size_t>(
            fill.final_ref_offset);
        if (fill.kind == FillKind::LeftExtend) {
            exact_query_start = static_cast<std::size_t>(
                fill.final_query_offset);
            exact_target_start = static_cast<std::size_t>(
                fill.final_ref_offset);
            exact_query_end = qe;
            exact_target_end = re;
        }
        fill.fill_edit_distance =
            exact_edit_distance_from_packed_cigar<Reverse>(
                query_seq, target_seq,
                exact_query_start, exact_query_end,
                exact_target_start, exact_target_end,
                std::span<const std::uint32_t>(result.cigar));
    }

    if (fill.cigar_scratch != nullptr && !result.cigar.empty()) {
        // Unpack the KSW2 packed CIGAR (length << 4 | opcode) into
        // CigarOp { length, opcode }.  Direct push without merging
        // adjacent same-opcode entries — the stitch phase merges across
        // fill boundaries instead.
        for (std::uint32_t entry : result.cigar) {
            const std::uint32_t opcode    = entry & 0xfU;
            const std::uint32_t op_length = entry >> 4;
            fill.cigar_scratch->push_back(CigarOp{op_length, opcode});
        }
    }
}

}  // namespace detail_ksw2_batched

/**
 * @brief Inter-sequence batched inner DP (DOC0254 §11.7).
 *
 * Per-lane scalar dispatcher — sequences whose query/target lengths
 * fit in the bucket bracket are processed lane-by-lane via the
 * existing intra-sequence striped KSW2 kernel.  The structural
 * batching is preserved so a future fused-lanes DP can substitute
 * here without touching the bucketing or stitching infrastructure.
 */
template <std::size_t Lanes, bool Reverse,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
void ksw2_extz2_batched_inner(
    FillDescriptor*                    fills,
    std::size_t                        n_fills,
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>&  query_seq,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>&  target_seq,
    const ScoringMatrix<QR>&           scoring_matrix,
    const Minimap2Params&              params,
    std::int32_t                       bucket_max_q,
    std::int32_t                       bucket_max_t,
    std::int32_t                       bucket_max_bw,
    Ksw2BatchedExecutionProof*         execution_proof)
{
    (void)bucket_max_q;
    (void)bucket_max_t;
    (void)bucket_max_bw;
    const std::int32_t align_bandwidth =
        static_cast<std::int32_t>(params.bandwidth * 1.5 + 1.0);
    for (std::size_t l = 0; l < n_fills && l < Lanes; ++l) {
        detail_ksw2_batched::run_single_fill<Reverse>(
            fills[l], query_seq, target_seq,
            scoring_matrix, params, align_bandwidth,
            execution_proof, /*long_fill=*/false);
    }
}

// ============================================================================
// DOC0254 §11.5 — Per-bucket execute phase.
// ============================================================================

template <supports_compact_sequence_range QR, supports_compact_sequence_range TR,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
void execute_buckets(
    std::span<std::pair<FillBucketKey, std::span<FillDescriptor>>> buckets,
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>&  query_seq,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>&  target_seq,
    const ScoringMatrix<QR>&           scoring_matrix,
    const Minimap2Params&              params,
    Ksw2BatchedExecutionProof*         execution_proof)
{
    using D = hn::ScalableTag<std::int16_t>;
    constexpr std::size_t Lanes = hn::MaxLanes(D{});

    const std::int32_t align_bandwidth =
        static_cast<std::int32_t>(params.bandwidth * 1.5 + 1.0);

    for (auto& [bucket_key, bucket_fills] : buckets) {
        const bool reverse = bucket_key.is_reverse;
        if (bucket_key.is_long_fill) {
            // Long fills route through the intra-sequence striped KSW2
            // kernel per fill — same code path the unbatched kernel uses.
            for (auto& fill : bucket_fills) {
                if (reverse) {
                    detail_ksw2_batched::run_single_fill<true>(
                        fill, query_seq, target_seq,
                        scoring_matrix, params, align_bandwidth,
                        execution_proof, /*long_fill=*/true);
                } else {
                    detail_ksw2_batched::run_single_fill<false>(
                        fill, query_seq, target_seq,
                        scoring_matrix, params, align_bandwidth,
                        execution_proof, /*long_fill=*/true);
                }
            }
            continue;
        }
        for (std::size_t i = 0; i < bucket_fills.size(); i += Lanes) {
            const std::size_t lane_count = std::min(Lanes,
                bucket_fills.size() - i);
            if (execution_proof != nullptr) {
                execution_proof->record_short_bucket_dispatch(
                    static_cast<std::uint64_t>(lane_count));
            }
            if (reverse) {
                ksw2_extz2_batched_inner<Lanes, true, QR, TR>(
                    bucket_fills.data() + i, lane_count,
                    query_seq, target_seq, scoring_matrix, params,
                    bucket_key.q_max, bucket_key.t_max, bucket_key.bw_max,
                    execution_proof);
            } else {
                ksw2_extz2_batched_inner<Lanes, false, QR, TR>(
                    bucket_fills.data() + i, lane_count,
                    query_seq, target_seq, scoring_matrix, params,
                    bucket_key.q_max, bucket_key.t_max, bucket_key.bw_max,
                    execution_proof);
            }
        }
    }
}

// ============================================================================
// DOC0254 §11.6 — CIGAR stitching.
// ============================================================================

inline void stitch_cigars_into_regions(
    std::span<AlignRegion>                          regions,
    std::span<const ::bioseq::chaining::Anchor>      anchors,
    ::bioseq::chaining::ArenaSlab<FillDescriptor>&  fills,
    std::int32_t                                    query_length,
    bool                                            emit_cigar)
{
    struct ChainOrder {
        [[nodiscard, gnu::always_inline]] inline bool operator()(
            const FillDescriptor& a, const FillDescriptor& b) const noexcept
        {
            if (a.region_idx != b.region_idx) return a.region_idx < b.region_idx;
            return a.fill_order < b.fill_order;
        }
    };
    std::stable_sort(fills.data(), fills.data() + fills.size(), ChainOrder{});

    std::size_t fill_index = 0;
    const sequence_size_t query_extent =
        static_cast<sequence_size_t>(query_length);
    for (std::size_t region_index = 0;
         region_index < regions.size(); ++region_index) {
        auto& region = regions[region_index];
        sequence_size_t final_ref_start = region.ref_start;
        sequence_size_t final_ref_end = region.ref_end;
        sequence_size_t final_query_start = region.is_reverse
            ? query_extent - region.query_end
            : region.query_start;
        sequence_size_t final_query_end = region.is_reverse
            ? query_extent - region.query_start
            : region.query_end;
        bool implicit_seed_appended = false;
        bool left_zdropped = false;
        bool gap_zdropped = false;

        while (fill_index < fills.size()
               && fills.data()[fill_index].region_idx
                    < static_cast<std::int32_t>(region_index)) {
            ++fill_index;
        }
        while (fill_index < fills.size()
               && fills.data()[fill_index].region_idx
                    == static_cast<std::int32_t>(region_index)) {
            const auto& fill = fills.data()[fill_index++];
            if (fill.kind == FillKind::RightExtend
                && region.seed_count == 1
                && !implicit_seed_appended
                && emit_cigar) {
                const auto& anchor =
                    anchors[static_cast<std::size_t>(region.anchor_start)];
                const std::size_t span =
                    static_cast<std::size_t>(anchor.query_span);
                const std::size_t query_end =
                    static_cast<std::size_t>(anchor.query_position) + 1u;
                const std::size_t target_end =
                    static_cast<std::size_t>(anchor.ref_position) + 1u;
                append_implicit_seed_cigar(
                    region.cigar,
                    std::min(query_end, span),
                    std::min(target_end, span));
                implicit_seed_appended = true;
            }

            const bool suppress_fill =
                (fill.kind == FillKind::GapBetween && gap_zdropped)
                || (fill.kind == FillKind::RightExtend
                    && (region.seed_count == 1
                        ? left_zdropped
                        : gap_zdropped));
            if (suppress_fill) continue;

            if (fill.cigar_scratch != nullptr) {
                const auto& scratch = *fill.cigar_scratch;
                for (const CigarOp op : scratch) {
                    if (!region.cigar.empty()
                        && region.cigar.back().opcode == op.opcode) {
                        region.cigar.back().length += op.length;
                    } else {
                        region.cigar.push_back(op);
                    }
                }
            }
            region.dp_score += fill.fill_score;
            region.edit_distance += fill.fill_edit_distance;

            if (fill.kind == FillKind::LeftExtend) {
                final_ref_start = static_cast<sequence_size_t>(
                    fill.final_ref_offset);
                final_query_start = static_cast<sequence_size_t>(
                    fill.final_query_offset);
                left_zdropped = fill.zdropped;
            } else {
                final_ref_end = static_cast<sequence_size_t>(
                    fill.final_ref_offset);
                final_query_end = static_cast<sequence_size_t>(
                    fill.final_query_offset);
                if (fill.kind == FillKind::GapBetween) {
                    gap_zdropped = fill.zdropped;
                }
            }
        }

        if (region.seed_count == 1 && !implicit_seed_appended && emit_cigar) {
            const auto& anchor =
                anchors[static_cast<std::size_t>(region.anchor_start)];
            const std::size_t span =
                static_cast<std::size_t>(anchor.query_span);
            const std::size_t query_end =
                static_cast<std::size_t>(anchor.query_position) + 1u;
            const std::size_t target_end =
                static_cast<std::size_t>(anchor.ref_position) + 1u;
            append_implicit_seed_cigar(
                region.cigar,
                std::min(query_end, span),
                std::min(target_end, span));
        }

        region.ref_start = final_ref_start;
        region.ref_end = final_ref_end;
        if (region.is_reverse) {
            region.query_start = query_extent - final_query_end;
            region.query_end = query_extent - final_query_start;
        } else {
            region.query_start = final_query_start;
            region.query_end = final_query_end;
        }
    }
}

// ============================================================================
// DOC0254 §11.3 — Phase A: fill collection.  Walks each region's chain
// and emits one FillDescriptor per (left-extend, gap-between,
// right-extend) phase.  Coordinates mirror the per-fill setup inside
// `align_regions<KSW2>` exactly so the batched and intra-sequence kernels
// agree on per-fill geometry.
// ============================================================================

namespace detail_ksw2_batched {

[[gnu::always_inline]] inline FillDescriptor* push_fill(
    ::bioseq::chaining::ArenaSlab<FillDescriptor>& fills,
    ::context::context&          /*ctx*/,
    std::int32_t                                   region_idx,
    std::int32_t                                   fill_order,
    FillKind                                       kind,
    bool                                           is_reverse,
    std::int32_t                                   query_offset,
    std::int32_t                                   target_offset,
    std::int32_t                                   query_len,
    std::int32_t                                   target_len,
    std::int32_t                                   bandwidth)
{
    FillDescriptor desc{};
    desc.region_idx        = region_idx;
    desc.fill_order        = fill_order;
    desc.kind              = kind;
    desc.is_reverse        = is_reverse;
    desc.query_offset      = query_offset;
    desc.target_offset     = target_offset;
    desc.query_len         = query_len;
    desc.target_len        = target_len;
    desc.bandwidth         = bandwidth;
    desc.cigar_scratch     = nullptr;
    desc.fill_score        = 0;
    desc.fill_edit_distance= 0;
    desc.final_ref_offset  = 0;
    desc.final_query_offset= 0;
    desc.zdropped          = false;
    fills.push_back(desc);
    return &fills.data()[fills.size() - 1];
}

}  // namespace detail_ksw2_batched

/**
 * @brief Top-level inter-region batched KSW2 entry.  Trait's `call`
 * forwards to this; the per-region inner loop mirrors
 * `align_regions<KSW2>` exactly on every per-fill coordinate.
 */
template <supports_compact_sequence_range QR, supports_compact_sequence_range TR,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
void align_regions_batched(
    std::span<AlignRegion>                       regions,
    std::span<const ::bioseq::chaining::Anchor>  anchors,
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>&            query_seq,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>&            target_seq,
    const ScoringMatrix<QR>&                     scoring_matrix,
    const Minimap2Params&                        params,
    Ksw2BatchedExecutionProof* execution_proof = nullptr)
{
    if (execution_proof != nullptr) {
        *execution_proof = {};
        execution_proof->record_invocation(
            static_cast<std::uint64_t>(regions.size()));
    }
    auto& ctx = ::context::current();
    auto chain_scope = ctx.push<::context::Arena::Chain>();

    const std::int32_t query_length  = static_cast<std::int32_t>(query_seq.bp_size());
    const std::int32_t target_length = static_cast<std::int32_t>(target_seq.bp_size());
    const std::int32_t align_bandwidth =
        static_cast<std::int32_t>(params.bandwidth * 1.5 + 1.0);

    auto fills = ::bioseq::chaining::make_arena_slab<FillDescriptor>();
    fills.reserve(detail_ksw2_batched::estimated_fill_count(regions));

    // Per-fill cigar scratch slabs.  The slab object itself is not
    // trivially-copyable (it owns a chain-arena pointer + destructor),
    // so the outer container is a plain std::vector that owns the slab
    // wrappers; the slab payloads still live in the chain arena and are
    // reclaimed at the end of this scope.
    std::vector<::bioseq::chaining::ArenaSlab<CigarOp>> cigar_scratch_pool;
    cigar_scratch_pool.reserve(detail_ksw2_batched::estimated_fill_count(regions));

    for (std::int32_t region_idx = 0;
         region_idx < static_cast<std::int32_t>(regions.size()); ++region_idx)
    {
        auto& region = regions[region_idx];
        if (region.seed_count <= 0) continue;

        const std::int32_t anchor_begin = region.anchor_start;
        const ::bioseq::chaining::Anchor& first_anchor = anchors[anchor_begin];
        const ::bioseq::chaining::Anchor& last_anchor  = anchors[anchor_begin + region.seed_count - 1];

        const std::int32_t first_span =
            static_cast<std::int32_t>(first_anchor.query_span);
        std::int32_t seed_ref_start =
            static_cast<std::int32_t>(first_anchor.ref_position) + 1 - first_span;
        std::int32_t seed_query_start =
            static_cast<std::int32_t>(first_anchor.query_position) + 1 - first_span;
        const std::int32_t seed_ref_end =
            static_cast<std::int32_t>(last_anchor.ref_position) + 1;
        const std::int32_t seed_query_end =
            static_cast<std::int32_t>(last_anchor.query_position) + 1;
        if (seed_ref_start   < 0) seed_ref_start   = 0;
        if (seed_query_start < 0) seed_query_start = 0;

        std::int32_t outer_ref_start   = seed_ref_start;
        std::int32_t outer_query_start = seed_query_start;
        std::int32_t outer_ref_end     = seed_ref_end;
        std::int32_t outer_query_end   = seed_query_end;

        if (seed_query_start > 0 && seed_ref_start > 0) {
            std::int32_t extension_len = std::min(seed_query_start, params.max_gap);
            outer_query_start = seed_query_start - extension_len;
            const std::int32_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min(extension_len, params.max_gap);
            extension_len = std::min(extension_len, seed_ref_start);
            outer_ref_start = seed_ref_start - extension_len;
        }
        if (seed_query_end < query_length && seed_ref_end < target_length) {
            std::int32_t extension_len =
                std::min(query_length - seed_query_end, params.max_gap);
            outer_query_end = seed_query_end + extension_len;
            const std::int32_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min(extension_len, params.max_gap);
            extension_len = std::min(extension_len, target_length - seed_ref_end);
            outer_ref_end = seed_ref_end + extension_len;
        }
        if (outer_query_end > query_length) outer_query_end = query_length;

        // DOC0254 §10.3 parity: reserve once from outer extents.
        region.cigar.reserve(
            detail_ksw2_batched::estimated_cigar_op_count(
                outer_query_end - outer_query_start,
                outer_ref_end   - outer_ref_start));
        region.cigar.clear();
        region.dp_score = 0;
        region.edit_distance = 0;

        if (params.compute_edit_distance && region.seed_count == 1) {
            region.edit_distance += region.is_reverse
                ? exact_edit_distance_for_implicit_seed<true>(
                    query_seq, target_seq,
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(seed_query_end),
                    static_cast<std::size_t>(seed_ref_start),
                    static_cast<std::size_t>(seed_ref_end))
                : exact_edit_distance_for_implicit_seed<false>(
                    query_seq, target_seq,
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(seed_query_end),
                    static_cast<std::size_t>(seed_ref_start),
                    static_cast<std::size_t>(seed_ref_end));
        }

        std::int32_t fill_order = 0;

        // --- Phase 1: left extension ---
        if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
            cigar_scratch_pool.push_back(
                ::bioseq::chaining::make_arena_slab<CigarOp>());
            auto* scratch_back = &cigar_scratch_pool[cigar_scratch_pool.size() - 1];
            scratch_back->reserve(static_cast<std::size_t>(
                std::max(seed_query_start - outer_query_start,
                         seed_ref_start   - outer_ref_start)) + 16u);
            FillDescriptor* d = detail_ksw2_batched::push_fill(
                fills, ctx, region_idx, fill_order++,
                FillKind::LeftExtend, region.is_reverse,
                outer_query_start, outer_ref_start,
                seed_query_start - outer_query_start,
                seed_ref_start   - outer_ref_start,
                align_bandwidth);
            d->cigar_scratch = scratch_back;
        }

        // --- Phase 2: gap-fills ---
        if (region.seed_count > 1) {
            std::int32_t qs = seed_query_start;
            std::int32_t rs = seed_ref_start;
            for (std::int32_t i = 1; i < region.seed_count; ++i) {
                const ::bioseq::chaining::Anchor& cur_anchor = anchors[anchor_begin + i];
                const std::int32_t qe =
                    static_cast<std::int32_t>(cur_anchor.query_position) + 1;
                const std::int32_t re =
                    static_cast<std::int32_t>(cur_anchor.ref_position) + 1;
                const bool is_last = (i == region.seed_count - 1);
                const std::int32_t gap_query = qe - qs;
                const std::int32_t gap_ref   = re - rs;
                if (!is_last
                    && (gap_query < params.min_ksw_len
                        || gap_ref < params.min_ksw_len))
                {
                    continue;
                }
                if (gap_query <= 0 || gap_ref <= 0) {
                    qs = qe; rs = re;
                    continue;
                }
                cigar_scratch_pool.push_back(
                    ::bioseq::chaining::make_arena_slab<CigarOp>());
                auto* scratch_back = &cigar_scratch_pool[cigar_scratch_pool.size() - 1];
                scratch_back->reserve(static_cast<std::size_t>(
                    std::max(gap_query, gap_ref)) + 16u);
                FillDescriptor* d = detail_ksw2_batched::push_fill(
                    fills, ctx, region_idx, fill_order++,
                    FillKind::GapBetween, region.is_reverse,
                    qs, rs, gap_query, gap_ref, align_bandwidth);
                d->cigar_scratch = scratch_back;
                qs = qe; rs = re;
            }
        }

        // --- Phase 3: right extension ---
        if (seed_query_end < outer_query_end && seed_ref_end < outer_ref_end) {
            cigar_scratch_pool.push_back(
                ::bioseq::chaining::make_arena_slab<CigarOp>());
            auto* scratch_back = &cigar_scratch_pool[cigar_scratch_pool.size() - 1];
            scratch_back->reserve(static_cast<std::size_t>(
                std::max(outer_query_end - seed_query_end,
                         outer_ref_end   - seed_ref_end)) + 16u);
            FillDescriptor* d = detail_ksw2_batched::push_fill(
                fills, ctx, region_idx, fill_order++,
                FillKind::RightExtend, region.is_reverse,
                seed_query_end, seed_ref_end,
                outer_query_end - seed_query_end,
                outer_ref_end   - seed_ref_end,
                align_bandwidth);
            d->cigar_scratch = scratch_back;
        }
    }

    // Phases B / C / D — bucketing, execution, stitch.
    if (execution_proof != nullptr) {
        execution_proof->record_collected_fills(
            static_cast<std::uint64_t>(fills.size()));
    }
    auto buckets = bucket_fills_by_geometry(fills, params);
    execute_buckets<QR, TR>(buckets, query_seq, target_seq,
                            scoring_matrix, params, execution_proof);
    stitch_cigars_into_regions(
        regions, anchors, fills, query_length,
        params.do_cigar || params.compute_edit_distance);

    // Update per-region coordinates and length fields — mirrors the
    // post-fill bookkeeping in `align_regions<KSW2>` (final-coordinates
    // derivation + match/block length tally).
    for (auto& region : regions) {
        if (region.seed_count <= 0) continue;
        region.dp_max_score = region.dp_score;
        region.has_alignment = true;

        std::int32_t cigar_match_count = 0;
        std::int32_t cigar_block_count = 0;
        for (const auto& cigar_op : region.cigar) {
            const std::int32_t op_len = static_cast<std::int32_t>(cigar_op.length);
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
            const std::int32_t effective_block =
                (cigar_block_count > 0) ? cigar_block_count : region.block_length;
            region.block_length = effective_block;
        }
    }
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
