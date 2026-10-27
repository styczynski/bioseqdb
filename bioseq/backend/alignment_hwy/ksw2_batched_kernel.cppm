/**
 * @file ksw2_batched_kernel.cppm
 * @brief Split alignment regions into KSW2 jobs and combine their results.
 *
 * A job, called a fill, covers an extension or a gap between anchors. Group fills
 * by size and orientation, run KSW2 for each one, then restore  chain order to
 * join their CIGARs and update region coordinates.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <limits>
#include <utility>

export module bioseq.highway.sources.ksw2_batched;

import std;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.highway.dispatch_contract;
import bioseq.highway.sources.ksw2;
import bioseq.highway.sources.alignment_utils;
import bioseq.highway.sources.simd_utils;
import bioseq.chaining.core;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/** Which part of an alignment region this fill covers. */
enum class fill_kind : std::uint8_t {
    left_extension  = 0,  ///< Extension preceding the first seed.
    gap_between     = 1,  ///< Gap between eed groups kept for alignment.
    right_extension = 2,  ///< Extension following the last seed.
};

/** Input interval, ordering and result storage for one KSW2 alignment job. */
struct alignas(16) fill_descriptor {
    std::int32_t query_offset;   ///< Inclusive query offset.
    std::int32_t target_offset;  ///< Inclusive target offset.
    std::int32_t query_length;   ///< Query length in symbols.
    std::int32_t target_length;  ///< Target length in symbols.
    std::int32_t bandwidth;      ///< Bandwidth used to group this fill.
    std::int32_t region_index;   ///< Index of the region that receives this result.
    std::int32_t fill_order;     ///< Fill order within the owning region.
    fill_kind     kind;          ///< Left extension, gap between anchors, or right extension.
    bool         is_reversed;    ///< Whether query ranks use reverse orientation.
    std::uint8_t reserved[2];    ///< Padding to keep the descriptor layout fixed.

    /// CIGAR operations owned by a slab in the chain  arena.
    /// Stitching appends them to the region in chain order.
    ::bioseq::chaining::arena_slab<bioseq::cigar_run>* cigar_scratch;

    /// The fill score is added to region.dp_score during stitching.
    std::int32_t fill_score;               ///< Alignment score for this fill.
    std::uint64_t fill_edit_distance;       ///< Edit count from replaying this fill's CIGAR, or zero when not requested.

    /// Endpoints used to update the region after alignment
    std::int32_t final_reference_offset;  ///< Target start for a left extension, or end for other fills.
    std::int32_t final_query_offset;      ///< Query start for a left extension, or end for other fills.
    bool         is_z_dropped;            ///< Whether Z-drop terminated the fill.

    std::uint8_t pad[3];  ///< Tail padding to keep the descriptor layout fixed.
};

/** Shared key for grouping fills by size, bandwidth and orientation. */
using fill_bucket_key =
    ::bioseq::simd::detail_ksw2_batched_ordering::fill_bucket_key;
/** Use  the shared fill-to-bucket mapping. */
using ::bioseq::simd::detail_ksw2_batched_ordering::bucket_key_for;

namespace detail_ksw2_batched {

/** Widen the configured bandwidth for alignment, rejecting integer overflow. */
[[nodiscard]] constexpr auto
try_calculate_alignment_bandwidth(std::int32_t configured_bandwidth,
                                  std::int32_t& alignment_bandwidth) noexcept -> bool {
    const std::int64_t scaled_bandwidth =
        static_cast<std::int64_t>(configured_bandwidth) * 3 / 2 + 1;
    if (scaled_bandwidth
            < static_cast<std::int64_t>(
                (std::numeric_limits<std::int32_t>::min)())
        || scaled_bandwidth
            > static_cast<std::int64_t>(
                (std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    alignment_bandwidth = static_cast<std::int32_t>(scaled_bandwidth);
    return true;
}

/** Check sequence lengths, region bounds and anchor ranges before batching. */
[[nodiscard]] inline auto
are_alignment_coordinates_supported(std::span<const alignment_region> regions,
                                    std::span<const ::bioseq::chaining::anchor> anchors,
                                    std::uint64_t query_extent,
                                    std::uint64_t target_extent) noexcept -> bool {
    constexpr std::uint64_t k_maximum_coordinate =
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)());
    if (query_extent > k_maximum_coordinate
        || target_extent > k_maximum_coordinate
        || regions.size() > k_maximum_coordinate) {
        return false;
    }

    for (const alignment_region& region : regions) {
        if (region.query_start > region.query_end
            || region.query_end > query_extent
            || region.ref_start > region.ref_end
            || region.ref_end > target_extent) {
            return false;
        }
        if (region.seed_count <= 0) continue;
        if (region.anchor_start < 0) return false;

        const std::size_t anchor_begin =
            static_cast<std::size_t>(region.anchor_start);
        const std::size_t anchor_count =
            static_cast<std::size_t>(region.seed_count);
        if (anchor_begin > anchors.size()
            || anchor_count > anchors.size() - anchor_begin) {
            return false;
        }
        for (std::size_t offset = 0; offset < anchor_count; ++offset) {
            const ::bioseq::chaining::anchor& current_anchor =
                anchors[anchor_begin + offset];
            if (current_anchor.query_position >= query_extent
                || current_anchor.ref_position >= target_extent) {
                return false;
            }
        }
    }
    return true;
}

/** Clear alignment-owned outputs after an unsupported-coordinate rejection. */
inline auto clear_unsupported_alignment_outputs(std::span<alignment_region> regions) noexcept
    -> void {
    for (alignment_region& region : regions) {
        region.dp_score = 0;
        region.dp_max_score = 0;
        region.has_alignment = false;
        region.cigar.clear();
        region.edit_distance = 0;
    }
}

/** Estimate descriptor capacity without traversing individual anchors. */
[[nodiscard]] inline auto estimated_fill_count(std::span<const alignment_region> regions) noexcept
    -> std::size_t {
    std::size_t total = 16;
    for (const auto& region : regions) {
        if (region.seed_count > 0) {
            total += 2u + static_cast<std::size_t>(region.seed_count);
        }
    }
    return total;
}

/** Estimate CIGAR space from both interval lengths, with room for extra runs. */
[[nodiscard, gnu::always_inline]] inline auto
estimated_cigar_operation_count(std::int32_t query_extent, std::int32_t reference_extent) noexcept
    -> std::size_t {
    const std::int32_t bounded_query_extent = query_extent > 0
        ? query_extent
        : 0;
    const std::int32_t bounded_reference_extent = reference_extent > 0
        ? reference_extent
        : 0;
    return static_cast<std::size_t>(bounded_query_extent)
        + static_cast<std::size_t>(bounded_reference_extent) + 16u;
}

}  // namespace detail_ksw2_batched

/** Groups fills by size and orientation, keeping their original order keys. */
[[nodiscard]] inline auto
bucket_fills_by_geometry(::bioseq::chaining::arena_slab<fill_descriptor>& fills,
                         std::int32_t default_bandwidth)
    -> std::vector<std::pair<fill_bucket_key, std::span<fill_descriptor>>> {
    std::stable_sort(fills.data(), fills.data() + fills.size(),
        ::bioseq::simd::detail_ksw2_batched_ordering::by_bucket_then_order{
            default_bandwidth});

    std::vector<std::pair<fill_bucket_key, std::span<fill_descriptor>>> buckets;
    if (fills.size() == 0) return buckets;
    buckets.reserve(16);

    std::size_t bucket_start = 0;
    fill_bucket_key current_bucket_key = bucket_key_for(fills.data()[0], default_bandwidth);
    for (std::size_t fill_index = 1; fill_index < fills.size(); ++fill_index) {
        const fill_bucket_key candidate_bucket_key = bucket_key_for(fills.data()[fill_index], default_bandwidth);
        if (!(candidate_bucket_key == current_bucket_key)) {
            buckets.push_back({current_bucket_key,
                std::span<fill_descriptor>{fills.data() + bucket_start, fill_index - bucket_start}});
            current_bucket_key = candidate_bucket_key;
            bucket_start = fill_index;
        }
    }
    buckets.push_back({current_bucket_key,
        std::span<fill_descriptor>{
            fills.data() + bucket_start, fills.size() - bucket_start}});
    return buckets;
}

// Each fill uses the single-pair KSW2 kereel. Short buckets are split into
// groups bounded by, the target's maximum SIMD lane count, and the fills run
// one after another. Their CIGARs remain  separate until stitching.

namespace detail_ksw2_batched {

/**
 * Align one extension or gap and save its score, endpoints and CIGAR.
 *
 * Choose one or two gap-penalty pairs from params. Left extensions read both
 * inputs backwards. When exact edit counts are requested, gaps between anchors
 * use unbanded alignment with Z-drop disabled, then count edits from the CIGAR.
 */
template <bool p_is_reversed, sequence_alphabet p_alphabet>
[[gnu::always_inline]] inline auto
align_single_fill(fill_descriptor& fill,
                  bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                  bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                  const scoring_matrix<p_alphabet>& scoring_matrix,
                  const minimap2_parameters& params,
                  std::int32_t alignment_bandwidth,
                  ksw2_batched_execution_stats* execution_stats,
                  bool is_long_fill) -> void {
    if (fill.query_length <= 0 || fill.target_length <= 0) {
        fill.fill_score = 0;
        fill.is_z_dropped = false;
        return;
    }

    if (execution_stats != nullptr) {
        execution_stats->record_nonempty_fill(
            static_cast<std::uint64_t>(fill.query_length),
            static_cast<std::uint64_t>(fill.target_length),
            is_long_fill);
    }

    ksw_flags flags{};
    int32_t end_bonus_for_fill = -1;
    if (fill.kind == fill_kind::left_extension) {
        flags.right_align    = true;
        flags.extz_only      = true;
        flags.reverse_cigar  = true;
        end_bonus_for_fill   = params.end_bonus;
    } else if (fill.kind == fill_kind::right_extension) {
        flags.extz_only      = true;
        end_bonus_for_fill   = params.end_bonus;
    } else {
        flags.approx_max     = true;
    }

    const std::size_t query_start = static_cast<std::size_t>(fill.query_offset);
    const std::size_t query_end = query_start + static_cast<std::size_t>(fill.query_length);
    const std::size_t target_start = static_cast<std::size_t>(fill.target_offset);
    const std::size_t target_end = target_start + static_cast<std::size_t>(fill.target_length);

    const int effective_bandwidth =
        params.compute_edit_distance && fill.kind == fill_kind::gap_between
            ? -1
            : alignment_bandwidth;
    const int effective_zdrop =
        params.compute_edit_distance && fill.kind == fill_kind::gap_between
            ? -1
            : params.zdrop;
    ksw_alignment_result result;
    if (params.gap_open1 == params.gap_open2
        && params.gap_ext1 == params.gap_ext2) {
        result = align_ksw2_single_affine<p_alphabet>(
            p_is_reversed, query_seq, target_seq, query_start, query_end, target_start, target_end,
            scoring_matrix,
            ksw_single_affine_parameters{
                .gap_open = static_cast<std::int8_t>(params.gap_open1),
                .gap_extend = static_cast<std::int8_t>(params.gap_ext1),
                .band_width = effective_bandwidth,
                .z_drop = effective_zdrop,
                .end_bonus = end_bonus_for_fill,
                .flags = flags,
            });
    } else {
        result = align_ksw2_dual_affine<p_alphabet>(
            p_is_reversed, query_seq, target_seq, query_start, query_end, target_start, target_end,
            scoring_matrix,
            ksw_dual_affine_parameters{
                .gap_open_short = static_cast<std::int8_t>(params.gap_open1),
                .gap_extend_short = static_cast<std::int8_t>(params.gap_ext1),
                .gap_open_long = static_cast<std::int8_t>(params.gap_open2),
                .gap_extend_long = static_cast<std::int8_t>(params.gap_ext2),
                .band_width = effective_bandwidth,
                .z_drop = effective_zdrop,
                .end_bonus = end_bonus_for_fill,
                .flags = flags,
            });
    }

    fill.is_z_dropped = result.is_z_dropped;
    if (fill.kind == fill_kind::gap_between) {
        if (result.is_z_dropped) {
            fill.final_reference_offset   = fill.target_offset + (result.max_t + 1);
            fill.final_query_offset = fill.query_offset  + (result.max_q + 1);
            fill.fill_score = result.max;
        } else {
            fill.final_reference_offset   = fill.target_offset + fill.target_length;
            fill.final_query_offset = fill.query_offset  + fill.query_length;
            fill.fill_score = result.score;
        }
    } else {
        fill.fill_score = result.max;
        if (fill.kind == fill_kind::left_extension) {
            const std::int32_t reference_offset = result.has_reached_end
                ? (result.mqe_t + 1) : (result.max_t + 1);
            const std::int32_t query_offset = result.has_reached_end
                ? fill.query_length : (result.max_q + 1);
            fill.final_reference_offset   = fill.target_offset + fill.target_length - reference_offset;
            fill.final_query_offset = fill.query_offset  + fill.query_length  - query_offset;
        } else {
            const std::int32_t reference_offset = result.has_reached_end
                ? (result.mqe_t + 1) : (result.max_t + 1);
            const std::int32_t query_offset = result.has_reached_end
                ? fill.query_length : (result.max_q + 1);
            fill.final_reference_offset   = fill.target_offset + reference_offset;
            fill.final_query_offset = fill.query_offset  + query_offset;
        }
    }

    if (params.compute_edit_distance) {
        std::size_t exact_query_start = query_start;
        std::size_t exact_target_start = target_start;
        std::size_t exact_query_end = static_cast<std::size_t>(
            fill.final_query_offset);
        std::size_t exact_target_end = static_cast<std::size_t>(
            fill.final_reference_offset);
        if (fill.kind == fill_kind::left_extension) {
            exact_query_start = static_cast<std::size_t>(
                fill.final_query_offset);
            exact_target_start = static_cast<std::size_t>(
                fill.final_reference_offset);
            exact_query_end = query_end;
            exact_target_end = target_end;
        }
        const auto exact_edits =
            exact_edit_distance_from_packed_cigar<p_is_reversed>(
                query_seq, target_seq,
                exact_query_start, exact_query_end,
                exact_target_start, exact_target_end,
                std::span<const std::uint32_t>(result.cigar), &count_equal_rank_chunk);
        if (!exact_edits) {
            const auto error = exact_edits.error();
            throw std::runtime_error{std::format(
                "batched exact edit-distance CIGAR failed (code={}, run={})",
                std::to_underlying(error.code), error.input_offset)};
        }
        fill.fill_edit_distance = *exact_edits;
    }

    if (fill.cigar_scratch != nullptr && !result.cigar.empty()) {
        // Decode and canonicalize through the common native-format boundary.
        if (!bioseq::decode_native_cigar(
                std::span<const std::uint32_t>{result.cigar.data(), result.cigar.size()},
                *fill.cigar_scratch)) {
            throw std::runtime_error{"batched alignment produced an invalid CIGAR"};
        }
    }
}

}  // namespace detail_ksw2_batched

/**
 * Run a short group of fills, up to p_lane_count entries.
 *
 * Each fill calls the single-pair KSW2 kernel in  sequence The bucket size
 * limits are unused here. Each result stays in its own descriptor for stitching.
 */
template <std::size_t p_lane_count, bool p_is_reversed, sequence_alphabet p_alphabet>
auto align_ksw2_fill_batch(fill_descriptor* fills,
                           std::size_t fill_count,
                           bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                           bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                           const scoring_matrix<p_alphabet>& scoring_matrix,
                           const minimap2_parameters& params,
                           std::int32_t alignment_bandwidth,
                           std::int32_t maximum_bucket_query_length,
                           std::int32_t maximum_bucket_target_length,
                           std::int32_t maximum_bucket_bandwidth,
                           ksw2_batched_execution_stats* execution_stats) -> void {
    (void)maximum_bucket_query_length;
    (void)maximum_bucket_target_length;
    (void)maximum_bucket_bandwidth;
    for (std::size_t lane_index = 0;
         lane_index < fill_count && lane_index < p_lane_count;
         ++lane_index) {
        detail_ksw2_batched::align_single_fill<p_is_reversed>(
            fills[lane_index], query_seq, target_seq,
            scoring_matrix, params, alignment_bandwidth,
            execution_stats, /*is_long_fill=*/false);
    }
}

/** Run all fill buckets with the selected query orientation. */
template <sequence_alphabet p_alphabet>
auto align_fill_buckets(std::span<std::pair<fill_bucket_key, std::span<fill_descriptor>>> buckets,
                        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                        const scoring_matrix<p_alphabet>& scoring_matrix,
                        const minimap2_parameters& params,
                        std::int32_t alignment_bandwidth,
                        ksw2_batched_execution_stats* execution_stats) -> void {
    using simd_tag_type = hn::ScalableTag<std::int16_t>;
    constexpr std::size_t k_lane_count = hn::MaxLanes(simd_tag_type{});

    for (auto& [bucket_key, bucket_fills] : buckets) {
        const bool is_reversed = bucket_key.is_reversed;
        if (bucket_key.is_long_fill) {
            // Run each long fill through the single-pair KSW2 kernel.
            for (auto& fill : bucket_fills) {
                if (is_reversed) {
                    detail_ksw2_batched::align_single_fill<true>(
                        fill, query_seq, target_seq,
                        scoring_matrix, params, alignment_bandwidth,
                        execution_stats, /*is_long_fill=*/true);
                } else {
                    detail_ksw2_batched::align_single_fill<false>(
                        fill, query_seq, target_seq,
                        scoring_matrix, params, alignment_bandwidth,
                        execution_stats, /*is_long_fill=*/true);
                }
            }
            continue;
        }
        for (std::size_t fill_index = 0;
             fill_index < bucket_fills.size();
             fill_index += k_lane_count) {
            const std::size_t lane_count = std::min(
                k_lane_count,
                bucket_fills.size() - fill_index);
            if (execution_stats != nullptr) {
                execution_stats->record_short_bucket_dispatch(
                    static_cast<std::uint64_t>(lane_count));
            }
            if (is_reversed) {
                align_ksw2_fill_batch<k_lane_count, true, p_alphabet>(
                    bucket_fills.data() + fill_index, lane_count,
                    query_seq, target_seq, scoring_matrix, params,
                    alignment_bandwidth,
                    bucket_key.maximum_query_length, bucket_key.maximum_target_length, bucket_key.maximum_bandwidth,
                    execution_stats);
            } else {
                align_ksw2_fill_batch<k_lane_count, false, p_alphabet>(
                    bucket_fills.data() + fill_index, lane_count,
                    query_seq, target_seq, scoring_matrix, params,
                    alignment_bandwidth,
                    bucket_key.maximum_query_length, bucket_key.maximum_target_length, bucket_key.maximum_bandwidth,
                    execution_stats);
            }
        }
    }
}

/**
 * Combine fill results into their regions in chain order.
 *
 * Merge adjacent CIGAR runs, sum scores and edit counts, and update endpoints.
 * After a gap stops at Z-drop, omit lyter gaps and the right extension.
 * A single-seed region also omits its right extension if the left one stops.
 * Convert  reverse-strand query coordinates back to the original sequence.
 */
inline auto stitch_cigars_into_regions(std::span<alignment_region> regions,
                                       std::span<const ::bioseq::chaining::anchor> anchors,
                                       ::bioseq::chaining::arena_slab<fill_descriptor>& fills,
                                       std::int32_t query_length,
                                       bool should_emit_cigar) -> void {
    std::stable_sort(fills.data(), fills.data() + fills.size(),
        ::bioseq::simd::detail_ksw2_batched_ordering::chain_order{});

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
        bool left_is_z_dropped = false;
        bool gap_is_z_dropped = false;

        while (fill_index < fills.size()
               && fills.data()[fill_index].region_index
                    < static_cast<std::int32_t>(region_index)) {
            ++fill_index;
        }
        while (fill_index < fills.size()
               && fills.data()[fill_index].region_index
                    == static_cast<std::int32_t>(region_index)) {
            const auto& fill = fills.data()[fill_index++];
            if (fill.kind == fill_kind::right_extension
                && region.seed_count == 1
                && !implicit_seed_appended
                && should_emit_cigar) {
                const auto& anchor =
                    anchors[static_cast<std::size_t>(region.anchor_start)];
                const std::size_t span =
                    static_cast<std::size_t>(anchor.query_span);
                const std::size_t query_end =
                    static_cast<std::size_t>(anchor.query_position) + 1u;
                const std::size_t target_end =
                    static_cast<std::size_t>(anchor.ref_position) + 1u;
                if (!append_implicit_seed_cigar(
                    region.cigar,
                    std::min(query_end, span),
                    std::min(target_end, span))) {
                    throw std::length_error{"batched seed CIGAR run is not representable"};
                }
                implicit_seed_appended = true;
            }

            const bool suppress_fill =
                (fill.kind == fill_kind::gap_between && gap_is_z_dropped)
                || (fill.kind == fill_kind::right_extension
                    && (region.seed_count == 1
                        ? left_is_z_dropped
                        : gap_is_z_dropped));
            if (suppress_fill) continue;

            if (fill.cigar_scratch != nullptr) {
                const auto& scratch = *fill.cigar_scratch;
                for (const bioseq::cigar_run op : scratch.as_const_span()) {
                    if (!bioseq::append_canonical_run(region.cigar, op)) {
                        throw std::length_error{"stitched CIGAR run is not representable"};
                    }
                }
            }
            region.dp_score += fill.fill_score;
            region.edit_distance += fill.fill_edit_distance;

            if (fill.kind == fill_kind::left_extension) {
                final_ref_start = static_cast<sequence_size_t>(
                    fill.final_reference_offset);
                final_query_start = static_cast<sequence_size_t>(
                    fill.final_query_offset);
                left_is_z_dropped = fill.is_z_dropped;
            } else {
                final_ref_end = static_cast<sequence_size_t>(
                    fill.final_reference_offset);
                final_query_end = static_cast<sequence_size_t>(
                    fill.final_query_offset);
                if (fill.kind == fill_kind::gap_between) {
                    gap_is_z_dropped = fill.is_z_dropped;
                }
            }
        }

        if (region.seed_count == 1 && !implicit_seed_appended && should_emit_cigar) {
            const auto& anchor =
                anchors[static_cast<std::size_t>(region.anchor_start)];
            const std::size_t span =
                static_cast<std::size_t>(anchor.query_span);
            const std::size_t query_end =
                static_cast<std::size_t>(anchor.query_position) + 1u;
            const std::size_t target_end =
                static_cast<std::size_t>(anchor.ref_position) + 1u;
            if (!append_implicit_seed_cigar(
                region.cigar,
                std::min(query_end, span),
                std::min(target_end, span))) {
                throw std::length_error{"batched seed CIGAR run is not representable"};
            }
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

// Collect fills in chain order before grouping them for execution.

namespace detail_ksw2_batched {

/** Append a fully initialized fill descriptor to arena-backed storage. */
[[gnu::always_inline]] inline auto
append_fill(::bioseq::chaining::arena_slab<fill_descriptor>& fills,
            ::context::context& /*alignment_context*/,
            std::int32_t region_index,
            std::int32_t fill_order,
            fill_kind kind,
            bool is_reversed,
            std::int32_t query_offset,
            std::int32_t target_offset,
            std::int32_t query_length,
            std::int32_t target_length,
            std::int32_t bandwidth) -> fill_descriptor* {
    fill_descriptor descriptor{
        .query_offset = query_offset,
        .target_offset = target_offset,
        .query_length = query_length,
        .target_length = target_length,
        .bandwidth = bandwidth,
        .region_index = region_index,
        .fill_order = fill_order,
        .kind = kind,
        .is_reversed = is_reversed,
        .reserved = {},
        .cigar_scratch = nullptr,
        .fill_score = 0,
        .fill_edit_distance = 0,
        .final_reference_offset = 0,
        .final_query_offset = 0,
        .is_z_dropped = false,
        .pad = {},
    };
    fills.push_back(descriptor);
    return &fills.data()[fills.size() - 1];
}

}  // namespace detail_ksw2_batched

/**
 * Align all regions by splitting them into extensions and gaps between anchors.
 *
 * Collect the fills, group them by size and orientation, and run KSW2. Stitch
 * their results back, into each region in chain order, then update length
 * estimates from the CIGAR. Invalid coordinates or an overflowing bandwidth
 * clear the alignment outputs and return without  running the kernels.
 */
template <sequence_alphabet p_alphabet>
auto align_regions_with_batched_ksw2(std::span<alignment_region> regions,
                                     std::span<const ::bioseq::chaining::anchor> anchors,
                                     bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                                     bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                                     const scoring_matrix<p_alphabet>& scoring_matrix,
                                     const minimap2_parameters& params,
                                     ksw2_batched_execution_stats* execution_stats = nullptr)
    -> void {
    if (execution_stats != nullptr) {
        *execution_stats = {};
        execution_stats->record_invocation(
            static_cast<std::uint64_t>(regions.size()));
    }

    const std::uint64_t query_extent =
        static_cast<std::uint64_t>(query_seq.bp_size());
    const std::uint64_t target_extent =
        static_cast<std::uint64_t>(target_seq.bp_size());
    std::int32_t alignment_bandwidth = 0;
    if (!detail_ksw2_batched::try_calculate_alignment_bandwidth(
            params.bandwidth, alignment_bandwidth)
        || !detail_ksw2_batched::are_alignment_coordinates_supported(
            regions, anchors, query_extent, target_extent)) {
        detail_ksw2_batched::clear_unsupported_alignment_outputs(regions);
        return;
    }

    auto& alignment_context = ::context::current();
    auto chain_scope = alignment_context.push<::context::arena::chain>();

    const std::int32_t query_length =
        static_cast<std::int32_t>(query_extent);
    const std::int32_t target_length =
        static_cast<std::int32_t>(target_extent);

    auto fills = ::bioseq::chaining::make_arena_slab<fill_descriptor>();
    fills.reserve(detail_ksw2_batched::estimated_fill_count(regions));

    // Keep the CIGAR slab objects in an owning vector because they need
    // destructors. Their payloads use the chain arena and remain available
    // until all fills have been stitched into the region results.
    std::vector<::bioseq::chaining::arena_slab<bioseq::cigar_run>> cigar_scratch_pool;
    cigar_scratch_pool.reserve(detail_ksw2_batched::estimated_fill_count(regions));

    for (std::size_t region_index = 0;
         region_index < regions.size(); ++region_index)
    {
        auto& region = regions[region_index];
        if (region.seed_count <= 0) continue;

        const std::size_t anchor_begin =
            static_cast<std::size_t>(region.anchor_start);
        const ::bioseq::chaining::anchor& first_anchor = anchors[anchor_begin];
        const ::bioseq::chaining::anchor& last_anchor = anchors[
            anchor_begin + static_cast<std::size_t>(region.seed_count) - 1u];

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
            std::int32_t extension_length = std::min(seed_query_start, params.max_gap);
            outer_query_start = seed_query_start - extension_length;
            const std::int32_t bonus_extension =
                (extension_length * params.match_score > params.gap_open1)
                    ? (extension_length * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_length += bonus_extension;
            extension_length = std::min(extension_length, params.max_gap);
            extension_length = std::min(extension_length, seed_ref_start);
            outer_ref_start = seed_ref_start - extension_length;
        }
        if (seed_query_end < query_length && seed_ref_end < target_length) {
            std::int32_t extension_length =
                std::min(query_length - seed_query_end, params.max_gap);
            outer_query_end = seed_query_end + extension_length;
            const std::int32_t bonus_extension =
                (extension_length * params.match_score > params.gap_open1)
                    ? (extension_length * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_length += bonus_extension;
            extension_length = std::min(extension_length, params.max_gap);
            extension_length = std::min(extension_length, target_length - seed_ref_end);
            outer_ref_end = seed_ref_end + extension_length;
        }
        if (outer_query_end > query_length) outer_query_end = query_length;

        // Reserve CIGAR space for the full interval, including end extensions.
        region.cigar.reserve(
            detail_ksw2_batched::estimated_cigar_operation_count(
                outer_query_end - outer_query_start,
                outer_ref_end   - outer_ref_start));
        region.cigar.clear();
        region.dp_score = 0;
        region.edit_distance = 0;

        if (params.compute_edit_distance && region.seed_count == 1) {
            const auto exact_edits = region.is_reverse
                ? exact_edit_distance_for_implicit_seed<true>(
                    query_seq, target_seq,
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(seed_query_end),
                    static_cast<std::size_t>(seed_ref_start),
                    static_cast<std::size_t>(seed_ref_end), &count_equal_rank_chunk)
                : exact_edit_distance_for_implicit_seed<false>(
                    query_seq, target_seq,
                    static_cast<std::size_t>(seed_query_start),
                    static_cast<std::size_t>(seed_query_end),
                    static_cast<std::size_t>(seed_ref_start),
                    static_cast<std::size_t>(seed_ref_end), &count_equal_rank_chunk);
            if (!exact_edits) {
                const auto error = exact_edits.error();
                throw std::runtime_error{std::format(
                    "batched implicit seed accounting failed (code={}, run={})",
                    std::to_underlying(error.code), error.input_offset)};
            }
            region.edit_distance += *exact_edits;
        }

        std::int32_t fill_order = 0;

        // Collect the optional left-extension fill.
        if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
            cigar_scratch_pool.push_back(
                ::bioseq::chaining::make_arena_slab<bioseq::cigar_run>());
            auto* fill_cigar_scratch = &cigar_scratch_pool[cigar_scratch_pool.size() - 1];
            fill_cigar_scratch->reserve(static_cast<std::size_t>(
                std::max(seed_query_start - outer_query_start,
                         seed_ref_start   - outer_ref_start)) + 16u);
            fill_descriptor* fill =
                detail_ksw2_batched::append_fill(fills,
                                                 alignment_context,
                                                 static_cast<std::int32_t>(region_index),
                                                 fill_order++,
                                                 fill_kind::left_extension,
                                                 region.is_reverse,
                                                 outer_query_start,
                                                 outer_ref_start,
                                                 seed_query_start - outer_query_start,
                                                 seed_ref_start - outer_ref_start,
                                                 alignment_bandwidth);
            fill->cigar_scratch = fill_cigar_scratch;
        }

        // Collect fills between successive anchors.
        if (region.seed_count > 1) {
            std::int32_t gap_query_start = seed_query_start;
            std::int32_t gap_reference_start = seed_ref_start;
            for (std::int32_t seed_index = 1; seed_index < region.seed_count; ++seed_index) {
                const ::bioseq::chaining::anchor& current_anchor =
                    anchors[anchor_begin + static_cast<std::size_t>(seed_index)];
                const std::int32_t gap_query_end =
                    static_cast<std::int32_t>(current_anchor.query_position) + 1;
                const std::int32_t gap_reference_end =
                    static_cast<std::int32_t>(current_anchor.ref_position) + 1;
                const bool is_last_seed = (seed_index == region.seed_count - 1);
                const std::int32_t gap_query_length = gap_query_end - gap_query_start;
                const std::int32_t gap_reference_length = gap_reference_end - gap_reference_start;
                if (!is_last_seed && (gap_query_length < params.min_ksw_len ||
                                 gap_reference_length < params.min_ksw_len)) {
                    continue;
                }
                if (gap_query_length <= 0 || gap_reference_length <= 0) {
                    gap_query_start = gap_query_end;
                    gap_reference_start = gap_reference_end;
                    continue;
                }
                cigar_scratch_pool.push_back(
                    ::bioseq::chaining::make_arena_slab<bioseq::cigar_run>());
                auto* fill_cigar_scratch = &cigar_scratch_pool[cigar_scratch_pool.size() - 1];
                fill_cigar_scratch->reserve(
                    static_cast<std::size_t>(std::max(gap_query_length, gap_reference_length)) +
                    16u);
                fill_descriptor* fill =
                    detail_ksw2_batched::append_fill(fills,
                                                     alignment_context,
                                                     static_cast<std::int32_t>(region_index),
                                                     fill_order++,
                                                     fill_kind::gap_between,
                                                     region.is_reverse,
                                                     gap_query_start,
                                                     gap_reference_start,
                                                     gap_query_length,
                                                     gap_reference_length,
                                                     alignment_bandwidth);
                fill->cigar_scratch = fill_cigar_scratch;
                gap_query_start = gap_query_end;
                gap_reference_start = gap_reference_end;
            }
        }

        // Collect the optional right-extension fill.
        if (seed_query_end < outer_query_end && seed_ref_end < outer_ref_end) {
            cigar_scratch_pool.push_back(
                ::bioseq::chaining::make_arena_slab<bioseq::cigar_run>());
            auto* fill_cigar_scratch = &cigar_scratch_pool[cigar_scratch_pool.size() - 1];
            fill_cigar_scratch->reserve(static_cast<std::size_t>(
                std::max(outer_query_end - seed_query_end,
                         outer_ref_end   - seed_ref_end)) + 16u);
            fill_descriptor* fill =
                detail_ksw2_batched::append_fill(fills,
                                                 alignment_context,
                                                 static_cast<std::int32_t>(region_index),
                                                 fill_order++,
                                                 fill_kind::right_extension,
                                                 region.is_reverse,
                                                 seed_query_end,
                                                 seed_ref_end,
                                                 outer_query_end - seed_query_end,
                                                 outer_ref_end - seed_ref_end,
                                                 alignment_bandwidth);
            fill->cigar_scratch = fill_cigar_scratch;
        }
    }

    // Bucket, execute, and stitch the collected fills.
    if (execution_stats != nullptr) {
        execution_stats->record_collected_fills(
            static_cast<std::uint64_t>(fills.size()));
    }
    auto buckets = bucket_fills_by_geometry(fills, alignment_bandwidth);
    align_fill_buckets<p_alphabet>(buckets, query_seq, target_seq,
        scoring_matrix, params, alignment_bandwidth, execution_stats);
    stitch_cigars_into_regions(
        regions, anchors, fills, query_length,
        params.do_cigar || params.compute_edit_distance);

    // Mark aligned regions and count CIGAR M/= columns and block length.
    for (auto& region : regions) {
        if (region.seed_count <= 0) continue;
        region.dp_max_score = region.dp_score;
        region.has_alignment = true;

        const auto cigar_summary = bioseq::summarize_alignment_cigar(region.cigar);
        if (cigar_summary.matches > 0u) {
            region.match_length = cigar_summary.matches;
            if (cigar_summary.block_length > 0u) {
                region.block_length = cigar_summary.block_length;
            }
        }
    }
}

/** Run batched KSW2 using this CPU ingtruction set */
template <sequence_alphabet p_alphabet>
auto align_ksw2_batched(std::span<alignment_region> regions,
                        std::span<const ::bioseq::chaining::anchor> anchors,
                        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                        const scoring_matrix<p_alphabet>& scoring_matrix,
                        const minimap2_parameters& params,
                        ksw2_batched_execution_stats* execution_stats) -> void {
    align_regions_with_batched_ksw2<p_alphabet>(
        regions, anchors, query_seq, target_seq, scoring_matrix, params,
        execution_stats);
}

}  // namespace target
}  // namespace bioseq::simd
