/**
 * @file ksw2_kernel.cppm
 * @brief Exposes the KSW2 core, facade, and registered dispatch slots.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <span>
#include <algorithm>
#include <type_traits>

export module bioseq.highway.sources.ksw2;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.highway.sources.alignment_utils;
import bioseq.highway.sources.ksw2_primitives;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// align_ksw2_core — unified single/dual-affine banded extension alignment
// =============================================================================

/**
 * @brief Executes the banded KSW2 recurrence for one sequence pair.
 * @tparam p_mode Single- or dual-affine recurrence mode.
 * @tparam p_is_reversed Whether query ranks use reverse orientation.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_alphabet_dimension Matrix dimension including the sentinel rank.
 * @tparam p_partial_score_type Narrow difference-score lane type.
 * @tparam p_full_score_type Exact accumulated-score lane type.
 * @param[in] query_seq Query sequence facade.
 * @param[in] target_seq Target sequence facade.
 * @param[in] query_start Inclusive query coordinate.
 * @param[in] query_end Exclusive query coordinate.
 * @param[in] target_start Inclusive target coordinate.
 * @param[in] target_end Exclusive target coordinate.
 * @param[in] scoring_matrix Canonical scalar and shuffle score matrix.
 * @param[in] gap_open Short-gap open penalty.
 * @param[in] gap_extend Short-gap extension penalty.
 * @param[in] gap_open_long Long-gap open penalty.
 * @param[in] gap_extend_long Long-gap extension penalty.
 * @param[in] configured_band_width Configured maximum band width.
 * @param[in] z_drop Z-drop threshold.
 * @param[in] end_bonus Query-end bonus.
 * @param[in] flags Traceback and extension-mode flags.
 * @return Alignment score, endpoints, termination state, and optional CIGAR.
 */
template<
    ksw_aligner_mode p_mode,
    bool p_is_reversed,
    sequence_alphabet p_alphabet,
    size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u,
    class p_partial_score_type = int8_t,
    class p_full_score_type = int32_t>
[[nodiscard]] ksw_alignment_result align_ksw2_core(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const scoring_matrix<p_alphabet>& scoring_matrix,
    int8_t gap_open,
    int8_t gap_extend,
    int8_t gap_open_long,
    int8_t gap_extend_long,
    int configured_band_width,
    int z_drop,
    int end_bonus,
    ksw_flags flags)
{
    constexpr bool k_is_dual_affine =
        p_mode == ksw_aligner_mode::dual_affine;

    // --- SIMD tags ---------------------------------------------------------
    hn::ScalableTag<p_partial_score_type> simd_tag;
    hn::ScalableTag<p_full_score_type>    full_score_tag;

    const size_t byte_lane_count = hn::Lanes(simd_tag);
    const size_t score_lane_count = hn::Lanes(full_score_tag);
    constexpr size_t k_maximum_score_lanes = hn::MaxLanes(hn::ScalableTag<p_full_score_type>{});

    // Every internal buffer (DP state, backtrack matrix, exact-score row,
    // encoded sequences) lives in this scope. The CIGAR output lives on
    // the result struct's own vector (heap) and is built before the scope
    // unwinds, so the dangling-span hazard is contained here.
    auto align_scope = context::current().push<context::arena::alignment>();

    ksw_alignment_result result;

    if constexpr (p_alphabet_dimension <= 1) {
        return result;
    }

    const size_t query_length  = query_end - query_start;
    const size_t target_length = target_end - target_start;
    if (query_length == 0 || target_length == 0) return result;

    const int query_length_i32 = static_cast<int>(query_length);
    const int target_length_i32 = static_cast<int>(target_length);

    const int gap_open_plus_extend = gap_open + gap_extend;
    const bool should_compute_cigar = !flags.score_only;
    const bool should_use_approximate_maximum = flags.approx_max;

    // --- SIMD constant vectors ---------------------------------------------
    const auto zero_scores          = hn::Zero(simd_tag);
    const auto match_scores   = hn::Set(simd_tag, scoring_matrix.match_score());
    const auto mismatch_scores = hn::Set(simd_tag, scoring_matrix.mismatch_score());
    const auto wildcard_ranks = hn::Set(simd_tag, static_cast<p_partial_score_type>(p_alphabet_dimension - 1));

    const auto double_gap_costs = hn::Set(simd_tag,
        static_cast<p_partial_score_type>((gap_open + gap_extend) * 2));
    const auto gap_open_scores = hn::Set(simd_tag, gap_open);
    const auto maximum_plausible_scores = hn::Set(simd_tag,
        static_cast<p_partial_score_type>(scoring_matrix.match_score() + (gap_open + gap_extend) * 2));

    const auto short_gap_open_scores = hn::Set(simd_tag, gap_open);
    const auto long_gap_open_scores  = hn::Set(simd_tag, gap_open_long);
    const auto short_gap_costs = hn::Set(simd_tag,
        static_cast<p_partial_score_type>(gap_open + gap_extend));
    const auto long_gap_costs  = hn::Set(simd_tag,
        static_cast<p_partial_score_type>(gap_open_long + gap_extend_long));

    const int8_t wildcard_score = scoring_matrix.wildcard_score();
    auto wildcard_scores = hn::Set(
        simd_tag, static_cast<p_partial_score_type>(wildcard_score));
    if (wildcard_score == 0) {
        if constexpr (k_is_dual_affine) {
            wildcard_scores = hn::Set(
                simd_tag, static_cast<p_partial_score_type>(-gap_extend_long));
        } else {
            wildcard_scores = hn::Set(
                simd_tag, static_cast<p_partial_score_type>(-gap_extend));
        }
    }

    // Backtrack direction constants
    const auto short_horizontal_gap_direction = hn::Set(simd_tag, static_cast<p_partial_score_type>(1));
    const auto short_vertical_gap_direction = hn::Set(simd_tag, static_cast<p_partial_score_type>(2));
    const auto long_horizontal_gap_direction = hn::Set(simd_tag, static_cast<p_partial_score_type>(3));
    const auto long_vertical_gap_direction = hn::Set(simd_tag, static_cast<p_partial_score_type>(4));
    const auto short_horizontal_gap_continuation  = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x08));
    const auto short_vertical_gap_continuation  = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x10));
    const auto long_horizontal_gap_continuation = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x20));
    const auto long_vertical_gap_continuation = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x40));

    // --- Band width --------------------------------------------------------
    const int band_width = configured_band_width < 0
        ? std::max(target_length_i32, query_length_i32) : configured_band_width;
    const int left_band  = band_width;
    const int right_band = band_width;

    // --- Score range sanity check ------------------------------------------
    int minimum_matrix_score = scoring_matrix.mismatch_score();
    for (size_t score_index = 1; score_index < scoring_matrix.entries.size(); ++score_index) {
        if (scoring_matrix.entries[score_index] < minimum_matrix_score)
            minimum_matrix_score = scoring_matrix.entries[score_index];
    }
    if (-minimum_matrix_score > 2 * (gap_open + gap_extend)) return result;

    // --- Dual-affine long-gap threshold ------------------------------------
    [[maybe_unused]] int long_gap_threshold = 0;
    [[maybe_unused]] int long_gap_difference = 0;
    if constexpr (k_is_dual_affine) {
        if (gap_extend != gap_extend_long) {
            long_gap_threshold = (gap_open_long - gap_open)
                / (gap_extend - gap_extend_long) - 1;
            if (gap_open_long + gap_extend_long + long_gap_threshold * gap_extend_long
                > gap_open + gap_extend + long_gap_threshold * gap_extend)
            {
                ++long_gap_threshold;
            }
        }
        long_gap_difference = long_gap_threshold * (gap_extend - gap_extend_long)
                        - (gap_open_long - gap_open) - gap_extend_long;
    }

    // --- Block dimensions --------------------------------------------------
    int band_vector_capacity = query_length_i32 < target_length_i32 ? query_length_i32 : target_length_i32;
    band_vector_capacity = ((band_vector_capacity < band_width + 1
                            ? band_vector_capacity : band_width + 1)
                         + static_cast<int>(byte_lane_count) - 1)
                        / static_cast<int>(byte_lane_count) + 1;

    // --- State arena -------------------------------------------------------
    ksw_state_for<p_mode> state(target_length, query_length, byte_lane_count);

    if constexpr (k_is_dual_affine) {
        state.fill_leading_arrays(4,
            static_cast<uint8_t>(-gap_open - gap_extend));
        state.fill_array(state.gap_e2_offset_base,
            static_cast<uint8_t>(-gap_open_long - gap_extend_long));
        state.fill_array(state.gap_f2_offset_base,
            static_cast<uint8_t>(-gap_open_long - gap_extend_long));
    }

    // --- Exact max tracking ------------------------------------------------
    // Exact-score row goes in the align arena; it's re-read every anti-
    // diagonal but never survives the call.
    p_full_score_type* exact_score_row = nullptr;
    if (!should_use_approximate_maximum) {
        const size_t exact_score_row_length = static_cast<size_t>(state.target_vector_count()) * byte_lane_count;
        auto exact_row_span =
            context::current().alloc_align_array<p_full_score_type>(exact_score_row_length);
        std::fill(exact_row_span.begin(), exact_row_span.end(),
                  static_cast<p_full_score_type>(k_ksw_negative_infinity));
        exact_score_row = exact_row_span.data();
    }

    // --- Backtrack storage -------------------------------------------------
    ksw_backtrack_matrix backtrack;
    std::span<int32_t> anti_diagonal_band_starts{};
    std::span<int32_t> anti_diagonal_band_ends{};
    if (should_compute_cigar) {
        backtrack.allocate(query_length_i32 + target_length_i32 - 1, band_vector_capacity, byte_lane_count);
        anti_diagonal_band_starts = backtrack.band_starts();
        anti_diagonal_band_ends   = backtrack.band_ends();
    }

    // --- Prepare sequences -------------------------------------------------
    // The DP body reads a reversed query (qr in minimap2 notation) and an
    // ordered target (sf). The orientation-aware encoder writes both views
    // directly into their final DP-state buffers.
    //
    // KSW2 orientation matrix (qr/sf relative to the source region):
    //   normal:        qr = reverse(query),               sf = target
    //   reverse_cigar: qr = query (already aligned),      sf = reverse(target)
    //
    // The strand-template p_is_reversed flips the query letters (rc); the
    // write_reversed flag flips the destination ordering. Both axes
    // are independently controlled by the strand and ordering parameters.
    std::span<uint8_t> query_destination(state.query_reversed, query_length);
    std::span<uint8_t> target_destination(state.target_bytes, target_length);
    if (flags.reverse_cigar) {
        // qr stays in source order — no extra mirror.
        query_seq.template copy_ranks<p_is_reversed, false>(
            query_destination, query_start, query_end);
        // Target is reversed; no rc on target, so the second template arg is
        // the only orientation flip.
        target_seq.template copy_ranks<false, true>(
            target_destination, target_start, target_end);
    } else {
        // qr = reverse(encoded query). For a reversed strand the
        // existing <true, false> path produced rc(query); we want rc then
        // reverse-again, which is just complement-only. Captured as
        // <true, true> (see overload comment).
        query_seq.template copy_ranks<p_is_reversed, true>(
            query_destination, query_start, query_end);
        target_seq.template copy_ranks<false, false>(
            target_destination, target_start, target_end);
    }

    // --- Approximate max tracking ------------------------------------------
    p_full_score_type approx_score = 0;
    int approximate_score_target_position = 0;

    // --- Determine fusion eligibility (non-generic-score path) -------------
    const bool should_use_fused_score = !flags.generic_score;

    // ====================================================================
    // Main DP loop
    // ====================================================================
    const int byte_lane_count_i32 = static_cast<int>(byte_lane_count);
    int previous_band_start = -1;
    int previous_band_end   = -1;

    for (int anti_diagonal = 0; anti_diagonal < query_length_i32 + target_length_i32 - 1; ++anti_diagonal) {
        int band_start = 0;
        int band_end   = target_length_i32 - 1;

        if (band_start < anti_diagonal - query_length_i32 + 1)
            band_start = anti_diagonal - query_length_i32 + 1;
        if (band_end > anti_diagonal)
            band_end = anti_diagonal;
        if (band_start < (anti_diagonal - right_band + 1) >> 1)
            band_start = (anti_diagonal - right_band + 1) >> 1;
        if (band_end > (anti_diagonal + left_band) >> 1)
            band_end = (anti_diagonal + left_band) >> 1;
        if (band_start > band_end) {
            result.is_z_dropped = true;
            break;
        }
        const int unaligned_band_start = band_start;
        const int unaligned_band_end   = band_end;
        band_start = (band_start / byte_lane_count_i32) * byte_lane_count_i32;
        band_end   = ((band_end + byte_lane_count_i32) / byte_lane_count_i32) * byte_lane_count_i32 - 1;

        // --- Boundary conditions -------------------------------------------
        int8_t horizontal_gap_boundary_carry;
        int8_t horizontal_difference_boundary_carry;
        [[maybe_unused]] int8_t long_horizontal_gap_boundary_carry = 0;
        uint8_t* HWY_RESTRICT query_reversed_aligned = state.query_reversed
            + (query_length_i32 - 1 - anti_diagonal);

        if constexpr (!k_is_dual_affine) {
            uint8_t* unsigned_up_differences   = reinterpret_cast<uint8_t*>(state.diff_up_base);
            uint8_t* unsigned_left_differences = reinterpret_cast<uint8_t*>(state.diff_left_base);
            uint8_t* unsigned_horizontal_gap_offsets     = reinterpret_cast<uint8_t*>(state.gap_e_offset_base);
            uint8_t* unsigned_vertical_gap_offsets     = reinterpret_cast<uint8_t*>(state.gap_f_offset_base);

            if (band_start > 0) {
                if (band_start - 1 >= previous_band_start && band_start - 1 <= previous_band_end) {
                    horizontal_gap_boundary_carry = unsigned_horizontal_gap_offsets[band_start - 1];
                    horizontal_difference_boundary_carry = unsigned_left_differences[band_start - 1];
                } else {
                    horizontal_gap_boundary_carry = 0;
                    horizontal_difference_boundary_carry = 0;
                }
            } else {
                horizontal_gap_boundary_carry = 0;
                horizontal_difference_boundary_carry = anti_diagonal ? static_cast<int8_t>(gap_open) : 0;
            }
            if (band_end >= anti_diagonal) {
                unsigned_vertical_gap_offsets[anti_diagonal] = 0;
                unsigned_up_differences[anti_diagonal] = anti_diagonal
                    ? static_cast<uint8_t>(gap_open) : 0;
            }
        } else {
            if (band_start > 0) {
                if (band_start - 1 >= previous_band_start && band_start - 1 <= previous_band_end) {
                    horizontal_gap_boundary_carry  = state.gap_e_offset_base[band_start - 1];
                    long_horizontal_gap_boundary_carry = state.gap_e2_offset_base[band_start - 1];
                    horizontal_difference_boundary_carry  = state.diff_left_base[band_start - 1];
                } else {
                    horizontal_gap_boundary_carry  = static_cast<int8_t>(-gap_open - gap_extend);
                    long_horizontal_gap_boundary_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
                    horizontal_difference_boundary_carry  = static_cast<int8_t>(-gap_open - gap_extend);
                }
            } else {
                horizontal_gap_boundary_carry  = static_cast<int8_t>(-gap_open - gap_extend);
                long_horizontal_gap_boundary_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
                horizontal_difference_boundary_carry  = anti_diagonal == 0
                    ? static_cast<int8_t>(-gap_open - gap_extend)
                    : anti_diagonal < long_gap_threshold
                        ? static_cast<int8_t>(-gap_extend)
                        : anti_diagonal == long_gap_threshold
                            ? static_cast<int8_t>(long_gap_difference)
                            : static_cast<int8_t>(-gap_extend_long);
            }
            if (band_end >= anti_diagonal) {
                state.gap_f_offset_base[anti_diagonal]  =
                    static_cast<int8_t>(-gap_open - gap_extend);
                state.gap_f2_offset_base[anti_diagonal] =
                    static_cast<int8_t>(-gap_open_long - gap_extend_long);
                state.diff_up_base[anti_diagonal] = anti_diagonal == 0
                    ? static_cast<int8_t>(-gap_open - gap_extend)
                    : anti_diagonal < long_gap_threshold
                        ? static_cast<int8_t>(-gap_extend)
                        : anti_diagonal == long_gap_threshold
                            ? static_cast<int8_t>(long_gap_difference)
                            : static_cast<int8_t>(-gap_extend_long);
            }
        }

        // --- Compute per-cell scores (only for generic_score path) ---------
        // When !generic_score the scores are computed inline inside the DP
        // loop (diag_score fusion optimisation).
        if (flags.generic_score) {
            for (int target_position = unaligned_band_start; target_position <= unaligned_band_end; ++target_position) {
                state.diag_score_base[target_position] = scoring_matrix.at(
                    state.target_bytes[target_position], query_reversed_aligned[target_position]);
            }
        }

        // --- Boundary vectors ----------------------------------------------
        auto horizontal_gap_carry  = set_lane_zero(simd_tag, horizontal_gap_boundary_carry);
        auto horizontal_difference_carry  = set_lane_zero(simd_tag, horizontal_difference_boundary_carry);
        [[maybe_unused]] auto long_horizontal_gap_carry = set_lane_zero(simd_tag, long_horizontal_gap_boundary_carry);
        const int first_band_vector = band_start / byte_lane_count_i32;
        const int last_band_vector   = band_end / byte_lane_count_i32;

        // ================================================================
        // Core DP inner loop — three paths depending on CIGAR / alignment
        // ================================================================

        if (!should_compute_cigar) {
            // ---- Score-only path ------------------------------------------
            for (int band_vector_index = first_band_vector; band_vector_index <= last_band_vector; ++band_vector_index) {
                const int state_offset = band_vector_index * byte_lane_count_i32;
                auto cell = should_use_fused_score
                    ? load_ksw_dp_cell<p_mode, true>(simd_tag, state_offset, state,
                        query_reversed_aligned, horizontal_gap_carry, horizontal_difference_carry, long_horizontal_gap_carry,
                        double_gap_costs, match_scores,
                        mismatch_scores, wildcard_scores,
                        wildcard_ranks)
                    : load_ksw_dp_cell<p_mode, false>(simd_tag, state_offset, state,
                        query_reversed_aligned, horizontal_gap_carry, horizontal_difference_carry, long_horizontal_gap_carry,
                        double_gap_costs, match_scores,
                        mismatch_scores, wildcard_scores,
                        wildcard_ranks);
                reduce_ksw_dp_maximum<p_mode>(simd_tag, cell, maximum_plausible_scores,
                    match_scores);
                store_ksw_dp_cell<p_mode>(simd_tag, state_offset, cell, state,
                    gap_open_scores, short_gap_open_scores, long_gap_open_scores,
                    short_gap_costs, long_gap_costs, zero_scores);
            }
        } else if (!flags.right_align) {
            // ---- CIGAR, gap left-alignment ---------------------------------
            anti_diagonal_band_starts[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diagonal_band_ends  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int band_vector_index = first_band_vector; band_vector_index <= last_band_vector; ++band_vector_index) {
                const int state_offset = band_vector_index * byte_lane_count_i32;
                auto cell = should_use_fused_score
                    ? load_ksw_dp_cell<p_mode, true>(simd_tag, state_offset, state,
                        query_reversed_aligned, horizontal_gap_carry, horizontal_difference_carry, long_horizontal_gap_carry,
                        double_gap_costs, match_scores,
                        mismatch_scores, wildcard_scores,
                        wildcard_ranks)
                    : load_ksw_dp_cell<p_mode, false>(simd_tag, state_offset, state,
                        query_reversed_aligned, horizontal_gap_carry, horizontal_difference_carry, long_horizontal_gap_carry,
                        double_gap_costs, match_scores,
                        mismatch_scores, wildcard_scores,
                        wildcard_ranks);

                // Direction tracking — left-align tie-break
                auto backtrack_directions = hn::IfThenElseZero(
                    hn::Gt(cell.short_horizontal_gap_candidates, cell.selected_scores), short_horizontal_gap_direction);
                cell.selected_scores = hn::Max(cell.selected_scores, cell.short_horizontal_gap_candidates);

                if constexpr (!k_is_dual_affine) {
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.short_vertical_gap_candidates, cell.selected_scores), short_vertical_gap_direction, backtrack_directions);
                    cell.selected_scores = maximum_unsigned_bytes(simd_tag, cell.selected_scores, cell.short_vertical_gap_candidates);
                    cell.selected_scores = minimum_unsigned_bytes(simd_tag, cell.selected_scores, maximum_plausible_scores);
                } else {
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.short_vertical_gap_candidates, cell.selected_scores), short_vertical_gap_direction, backtrack_directions);
                    cell.selected_scores = hn::Max(cell.selected_scores, cell.short_vertical_gap_candidates);
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.long_horizontal_gap_candidates, cell.selected_scores), long_horizontal_gap_direction, backtrack_directions);
                    cell.selected_scores = hn::Max(cell.selected_scores, cell.long_horizontal_gap_candidates);
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.long_vertical_gap_candidates, cell.selected_scores), long_vertical_gap_direction, backtrack_directions);
                    cell.selected_scores = hn::Max(cell.selected_scores, cell.long_vertical_gap_candidates);
                    cell.selected_scores = hn::Min(cell.selected_scores, match_scores);
                }

                // Store diff + gap states
                hn::Store(hn::Sub(cell.selected_scores, cell.shifted_left_differences), simd_tag,
                          state.diff_up_base + state_offset);
                hn::Store(hn::Sub(cell.selected_scores, cell.current_up_differences), simd_tag,
                          state.diff_left_base + state_offset);

                if constexpr (!k_is_dual_affine) {
                    auto single_gap_open_threshold = hn::Sub(cell.selected_scores, gap_open_scores);
                    cell.short_horizontal_gap_candidates = hn::Sub(cell.short_horizontal_gap_candidates, single_gap_open_threshold);
                    cell.short_vertical_gap_candidates = hn::Sub(cell.short_vertical_gap_candidates, single_gap_open_threshold);

                    auto short_horizontal_gap_mask = hn::Gt(cell.short_horizontal_gap_candidates, zero_scores);
                    hn::Store(hn::IfThenElseZero(short_horizontal_gap_mask, cell.short_horizontal_gap_candidates), simd_tag,
                              state.gap_e_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenElseZero(short_horizontal_gap_mask, short_horizontal_gap_continuation));

                    auto short_vertical_gap_mask = hn::Gt(cell.short_vertical_gap_candidates, zero_scores);
                    hn::Store(hn::IfThenElseZero(short_vertical_gap_mask, cell.short_vertical_gap_candidates), simd_tag,
                              state.gap_f_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenElseZero(short_vertical_gap_mask, short_vertical_gap_continuation));
                } else {
                    auto short_gap_open_threshold = hn::Sub(cell.selected_scores, short_gap_open_scores);
                    cell.short_horizontal_gap_candidates = hn::Sub(cell.short_horizontal_gap_candidates, short_gap_open_threshold);
                    cell.short_vertical_gap_candidates = hn::Sub(cell.short_vertical_gap_candidates, short_gap_open_threshold);
                    auto long_gap_open_threshold = hn::Sub(cell.selected_scores, long_gap_open_scores);
                    cell.long_horizontal_gap_candidates = hn::Sub(cell.long_horizontal_gap_candidates, long_gap_open_threshold);
                    cell.long_vertical_gap_candidates = hn::Sub(cell.long_vertical_gap_candidates, long_gap_open_threshold);

                    auto short_horizontal_gap_mask = hn::Gt(cell.short_horizontal_gap_candidates, zero_scores);
                    hn::Store(hn::Sub(hn::IfThenElseZero(short_horizontal_gap_mask, cell.short_horizontal_gap_candidates),
                                       short_gap_costs),
                              simd_tag, state.gap_e_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenElseZero(short_horizontal_gap_mask, short_horizontal_gap_continuation));

                    auto short_vertical_gap_mask = hn::Gt(cell.short_vertical_gap_candidates, zero_scores);
                    hn::Store(hn::Sub(hn::IfThenElseZero(short_vertical_gap_mask, cell.short_vertical_gap_candidates),
                                       short_gap_costs),
                              simd_tag, state.gap_f_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenElseZero(short_vertical_gap_mask, short_vertical_gap_continuation));

                    auto long_horizontal_gap_mask = hn::Gt(cell.long_horizontal_gap_candidates, zero_scores);
                    hn::Store(hn::Sub(hn::IfThenElseZero(long_horizontal_gap_mask, cell.long_horizontal_gap_candidates),
                                       long_gap_costs),
                              simd_tag, state.gap_e2_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenElseZero(long_horizontal_gap_mask, long_horizontal_gap_continuation));

                    auto long_vertical_gap_mask = hn::Gt(cell.long_vertical_gap_candidates, zero_scores);
                    hn::Store(hn::Sub(hn::IfThenElseZero(long_vertical_gap_mask, cell.long_vertical_gap_candidates),
                                       long_gap_costs),
                              simd_tag, state.gap_f2_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenElseZero(long_vertical_gap_mask, long_vertical_gap_continuation));
                }

                hn::Store(backtrack_directions, simd_tag,
                    backtrack.vector_slot_bytes(anti_diagonal, band_vector_index - first_band_vector));
            }
        } else {
            // ---- CIGAR, gap right-alignment --------------------------------
            anti_diagonal_band_starts[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diagonal_band_ends  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int band_vector_index = first_band_vector; band_vector_index <= last_band_vector; ++band_vector_index) {
                const int state_offset = band_vector_index * byte_lane_count_i32;
                auto cell = should_use_fused_score
                    ? load_ksw_dp_cell<p_mode, true>(simd_tag, state_offset, state,
                        query_reversed_aligned, horizontal_gap_carry, horizontal_difference_carry, long_horizontal_gap_carry,
                        double_gap_costs, match_scores,
                        mismatch_scores, wildcard_scores,
                        wildcard_ranks)
                    : load_ksw_dp_cell<p_mode, false>(simd_tag, state_offset, state,
                        query_reversed_aligned, horizontal_gap_carry, horizontal_difference_carry, long_horizontal_gap_carry,
                        double_gap_costs, match_scores,
                        mismatch_scores, wildcard_scores,
                        wildcard_ranks);

                // Direction tracking — right-align tie-break
                auto backtrack_directions = hn::IfThenZeroElse(
                    hn::Gt(cell.selected_scores, cell.short_horizontal_gap_candidates), short_horizontal_gap_direction);
                cell.selected_scores = hn::Max(cell.selected_scores, cell.short_horizontal_gap_candidates);

                if constexpr (!k_is_dual_affine) {
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.selected_scores, cell.short_vertical_gap_candidates), backtrack_directions, short_vertical_gap_direction);
                    cell.selected_scores = maximum_unsigned_bytes(simd_tag, cell.selected_scores, cell.short_vertical_gap_candidates);
                    cell.selected_scores = minimum_unsigned_bytes(simd_tag, cell.selected_scores, maximum_plausible_scores);
                } else {
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.selected_scores, cell.short_vertical_gap_candidates), backtrack_directions, short_vertical_gap_direction);
                    cell.selected_scores = hn::Max(cell.selected_scores, cell.short_vertical_gap_candidates);
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.selected_scores, cell.long_horizontal_gap_candidates), backtrack_directions, long_horizontal_gap_direction);
                    cell.selected_scores = hn::Max(cell.selected_scores, cell.long_horizontal_gap_candidates);
                    backtrack_directions = hn::IfThenElse(
                        hn::Gt(cell.selected_scores, cell.long_vertical_gap_candidates), backtrack_directions, long_vertical_gap_direction);
                    cell.selected_scores = hn::Max(cell.selected_scores, cell.long_vertical_gap_candidates);
                    cell.selected_scores = hn::Min(cell.selected_scores, match_scores);
                }

                hn::Store(hn::Sub(cell.selected_scores, cell.shifted_left_differences), simd_tag,
                          state.diff_up_base + state_offset);
                hn::Store(hn::Sub(cell.selected_scores, cell.current_up_differences), simd_tag,
                          state.diff_left_base + state_offset);

                if constexpr (!k_is_dual_affine) {
                    auto single_gap_open_threshold = hn::Sub(cell.selected_scores, gap_open_scores);
                    cell.short_horizontal_gap_candidates = hn::Sub(cell.short_horizontal_gap_candidates, single_gap_open_threshold);
                    cell.short_vertical_gap_candidates = hn::Sub(cell.short_vertical_gap_candidates, single_gap_open_threshold);

                    auto short_horizontal_gap_is_negative = hn::Gt(zero_scores, cell.short_horizontal_gap_candidates);
                    hn::Store(hn::IfThenZeroElse(short_horizontal_gap_is_negative, cell.short_horizontal_gap_candidates), simd_tag,
                              state.gap_e_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenZeroElse(short_horizontal_gap_is_negative, short_horizontal_gap_continuation));

                    auto short_vertical_gap_is_negative = hn::Gt(zero_scores, cell.short_vertical_gap_candidates);
                    hn::Store(hn::IfThenZeroElse(short_vertical_gap_is_negative, cell.short_vertical_gap_candidates), simd_tag,
                              state.gap_f_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenZeroElse(short_vertical_gap_is_negative, short_vertical_gap_continuation));
                } else {
                    auto short_gap_open_threshold = hn::Sub(cell.selected_scores, short_gap_open_scores);
                    cell.short_horizontal_gap_candidates = hn::Sub(cell.short_horizontal_gap_candidates, short_gap_open_threshold);
                    cell.short_vertical_gap_candidates = hn::Sub(cell.short_vertical_gap_candidates, short_gap_open_threshold);
                    auto long_gap_open_threshold = hn::Sub(cell.selected_scores, long_gap_open_scores);
                    cell.long_horizontal_gap_candidates = hn::Sub(cell.long_horizontal_gap_candidates, long_gap_open_threshold);
                    cell.long_vertical_gap_candidates = hn::Sub(cell.long_vertical_gap_candidates, long_gap_open_threshold);

                    auto short_horizontal_gap_is_negative = hn::Gt(zero_scores, cell.short_horizontal_gap_candidates);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(short_horizontal_gap_is_negative, cell.short_horizontal_gap_candidates),
                                       short_gap_costs),
                              simd_tag, state.gap_e_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenZeroElse(short_horizontal_gap_is_negative, short_horizontal_gap_continuation));

                    auto short_vertical_gap_is_negative = hn::Gt(zero_scores, cell.short_vertical_gap_candidates);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(short_vertical_gap_is_negative, cell.short_vertical_gap_candidates),
                                       short_gap_costs),
                              simd_tag, state.gap_f_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenZeroElse(short_vertical_gap_is_negative, short_vertical_gap_continuation));

                    auto long_horizontal_gap_is_negative = hn::Gt(zero_scores, cell.long_horizontal_gap_candidates);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(long_horizontal_gap_is_negative, cell.long_horizontal_gap_candidates),
                                       long_gap_costs),
                              simd_tag, state.gap_e2_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenZeroElse(long_horizontal_gap_is_negative, long_horizontal_gap_continuation));

                    auto long_vertical_gap_is_negative = hn::Gt(zero_scores, cell.long_vertical_gap_candidates);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(long_vertical_gap_is_negative, cell.long_vertical_gap_candidates),
                                       long_gap_costs),
                              simd_tag, state.gap_f2_offset_base + state_offset);
                    backtrack_directions = hn::Or(backtrack_directions, hn::IfThenZeroElse(long_vertical_gap_is_negative, long_vertical_gap_continuation));
                }

                hn::Store(backtrack_directions, simd_tag,
                    backtrack.vector_slot_bytes(anti_diagonal, band_vector_index - first_band_vector));
            }
        }

        // ================================================================
        // Max score tracking and Z-drop
        // ================================================================
        const int8_t z_drop_gap_extend = k_is_dual_affine
            ? gap_extend_long
            : gap_extend;

        if (!should_use_approximate_maximum) {
            p_full_score_type anti_diagonal_maximum_score;
            int anti_diagonal_maximum_target_position;
            if (anti_diagonal > 0) {
                if constexpr (!k_is_dual_affine) {
                    const auto* unsigned_up_differences = reinterpret_cast<const uint8_t*>(state.diff_up_base);
                    const auto* unsigned_left_differences = reinterpret_cast<const uint8_t*>(state.diff_left_base);
                    anti_diagonal_maximum_score = exact_score_row[unaligned_band_end] = unaligned_band_end > 0
                        ? exact_score_row[unaligned_band_end - 1]
                          + static_cast<p_full_score_type>(unsigned_up_differences[unaligned_band_end]) - gap_open_plus_extend
                        : exact_score_row[unaligned_band_end]
                          + static_cast<p_full_score_type>(unsigned_left_differences[unaligned_band_end]) - gap_open_plus_extend;
                } else {
                    anti_diagonal_maximum_score = exact_score_row[unaligned_band_end] = unaligned_band_end > 0
                        ? exact_score_row[unaligned_band_end - 1]
                          + static_cast<p_full_score_type>(state.diff_up_base[unaligned_band_end])
                        : exact_score_row[unaligned_band_end]
                          + static_cast<p_full_score_type>(state.diff_left_base[unaligned_band_end]);
                }
                anti_diagonal_maximum_target_position = unaligned_band_end;

                auto maximum_score_vector =
                    hn::Set(full_score_tag, anti_diagonal_maximum_score);
                auto maximum_position_vector = hn::Set(
                    full_score_tag,
                    static_cast<p_full_score_type>(
                        anti_diagonal_maximum_target_position));
                [[maybe_unused]] auto gap_open_extend_vector = hn::Set(full_score_tag,
                    static_cast<p_full_score_type>(gap_open_plus_extend));

                const int block_end = unaligned_band_start
                    + (unaligned_band_end - unaligned_band_start) / static_cast<int>(score_lane_count)
                      * static_cast<int>(score_lane_count);
                int target_position = unaligned_band_start;
                for (;
                     target_position < block_end;
                     target_position += static_cast<int>(score_lane_count)) {
                    auto exact_scores = hn::LoadU(
                        full_score_tag,
                        &exact_score_row[target_position]);
                    HWY_ALIGN std::array<
                        p_full_score_type,
                        k_maximum_score_lanes> left_difference_values{};
                    for (size_t lane_index = 0;
                         lane_index < score_lane_count;
                         ++lane_index) {
                        if constexpr (!k_is_dual_affine)
                            left_difference_values[lane_index] =
                                static_cast<p_full_score_type>(
                                    reinterpret_cast<const uint8_t*>(
                                        state.diff_left_base)[
                                        target_position + lane_index]);
                        else
                            left_difference_values[lane_index] =
                                static_cast<p_full_score_type>(
                                    state.diff_left_base[
                                        target_position + lane_index]);
                    }
                    const auto left_difference_vector = hn::Load(
                        full_score_tag,
                        left_difference_values.data());
                    exact_scores = hn::Add(
                        exact_scores,
                        left_difference_vector);
                    if constexpr (!k_is_dual_affine) {
                        exact_scores = hn::Sub(
                            exact_scores,
                            gap_open_extend_vector);
                    }
                    hn::StoreU(
                        exact_scores,
                        full_score_tag,
                        &exact_score_row[target_position]);

                    const auto target_position_vector = hn::Set(
                        full_score_tag,
                        static_cast<p_full_score_type>(target_position));
                    const auto is_new_maximum = hn::Gt(
                        exact_scores,
                        maximum_score_vector);
                    maximum_score_vector = hn::IfThenElse(
                        is_new_maximum,
                        exact_scores,
                        maximum_score_vector);
                    maximum_position_vector = hn::IfThenElse(
                        is_new_maximum,
                        target_position_vector,
                        maximum_position_vector);
                }

                // Highway stores every active lane before either array is read.
                HWY_ALIGN std::array<
                    p_full_score_type,
                    k_maximum_score_lanes> maximum_scores;
                HWY_ALIGN std::array<
                    p_full_score_type,
                    k_maximum_score_lanes> maximum_positions;
                hn::Store(
                    maximum_score_vector,
                    full_score_tag,
                    maximum_scores.data());
                hn::Store(
                    maximum_position_vector,
                    full_score_tag,
                    maximum_positions.data());
                for (size_t lane_index = 0;
                     lane_index < score_lane_count;
                     ++lane_index) {
                    if (anti_diagonal_maximum_score < maximum_scores[lane_index]) {
                        anti_diagonal_maximum_score = maximum_scores[lane_index];
                        anti_diagonal_maximum_target_position =
                            static_cast<int>(maximum_positions[lane_index])
                            + static_cast<int>(lane_index);
                    }
                }
                for (;
                     target_position < unaligned_band_end;
                     ++target_position) {
                    if constexpr (!k_is_dual_affine) {
                        exact_score_row[target_position] +=
                            static_cast<p_full_score_type>(
                                reinterpret_cast<const uint8_t*>(
                                    state.diff_left_base)[target_position])
                            - gap_open_plus_extend;
                    } else {
                        exact_score_row[target_position] +=
                            static_cast<p_full_score_type>(
                                state.diff_left_base[target_position]);
                    }
                    if (exact_score_row[target_position]
                        > anti_diagonal_maximum_score) {
                        anti_diagonal_maximum_score = exact_score_row[target_position];
                        anti_diagonal_maximum_target_position = target_position;
                    }
                }
            } else {
                if constexpr (!k_is_dual_affine) {
                    exact_score_row[0] = static_cast<p_full_score_type>(
                        reinterpret_cast<const uint8_t*>(state.diff_left_base)[0])
                        - gap_open_plus_extend - gap_open_plus_extend;
                } else {
                    exact_score_row[0] = static_cast<p_full_score_type>(
                        state.diff_left_base[0]) - gap_open_plus_extend;
                }
                anti_diagonal_maximum_score = exact_score_row[0];
                anti_diagonal_maximum_target_position = 0;
            }
            if (unaligned_band_end == target_length_i32 - 1 && exact_score_row[unaligned_band_end] > result.mte) {
                result.mte = exact_score_row[unaligned_band_end];
                result.mte_q = anti_diagonal - unaligned_band_end;
            }
            if (anti_diagonal - unaligned_band_start == query_length_i32 - 1
                && exact_score_row[unaligned_band_start] > result.mqe)
            {
                result.mqe = exact_score_row[unaligned_band_start];
                result.mqe_t = unaligned_band_start;
            }
            if (apply_z_drop_heuristic(result, true,
                                       anti_diagonal_maximum_score, anti_diagonal,
                                       anti_diagonal_maximum_target_position,
                                       z_drop, z_drop_gap_extend))
            {
                break;
            }
            if (anti_diagonal == query_length_i32 + target_length_i32 - 2 && unaligned_band_end == target_length_i32 - 1) {
                result.score = exact_score_row[target_length_i32 - 1];
            }
        } else {
            // --- Approximate max -------------------------------------------
            if (anti_diagonal > 0) {
                if (approximate_score_target_position >= unaligned_band_start
                    && approximate_score_target_position <= unaligned_band_end
                    && approximate_score_target_position + 1 >= unaligned_band_start
                    && approximate_score_target_position + 1 <= unaligned_band_end)
                {
                    p_full_score_type delta_left, delta_up;
                    if constexpr (!k_is_dual_affine) {
                        delta_left = static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[approximate_score_target_position])
                            - gap_open_plus_extend;
                        delta_up = static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_up_base)[approximate_score_target_position + 1])
                            - gap_open_plus_extend;
                    } else {
                        delta_left = state.diff_left_base[approximate_score_target_position];
                        delta_up   = state.diff_up_base[approximate_score_target_position + 1];
                    }
                    if (delta_left > delta_up) {
                        approx_score += delta_left;
                    } else {
                        approx_score += delta_up;
                        ++approximate_score_target_position;
                    }
                } else if (approximate_score_target_position >= unaligned_band_start
                           && approximate_score_target_position <= unaligned_band_end)
                {
                    if constexpr (!k_is_dual_affine) {
                        approx_score += static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[approximate_score_target_position])
                            - gap_open_plus_extend;
                    } else {
                        approx_score += static_cast<p_full_score_type>(
                            state.diff_left_base[approximate_score_target_position]);
                    }
                } else {
                    ++approximate_score_target_position;
                    if constexpr (!k_is_dual_affine) {
                        approx_score += static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_up_base)[approximate_score_target_position])
                            - gap_open_plus_extend;
                    } else {
                        approx_score += static_cast<p_full_score_type>(
                            state.diff_up_base[approximate_score_target_position]);
                    }
                }
                if constexpr (!k_is_dual_affine) {
                    if (flags.approx_zdrop
                        && apply_z_drop_heuristic(result, true,
                                                  approx_score, anti_diagonal,
                                                  approximate_score_target_position,
                                                  z_drop, z_drop_gap_extend))
                    {
                        break;
                    }
                }
            } else {
                if constexpr (!k_is_dual_affine) {
                    approx_score = static_cast<p_full_score_type>(
                        reinterpret_cast<const uint8_t*>(state.diff_left_base)[0])
                        - gap_open_plus_extend - gap_open_plus_extend;
                } else {
                    approx_score = static_cast<p_full_score_type>(
                        state.diff_left_base[0]) - gap_open_plus_extend;
                }
                approximate_score_target_position = 0;
            }
            if constexpr (k_is_dual_affine) {
                if (flags.approx_zdrop
                    && apply_z_drop_heuristic(result, true,
                                              approx_score, anti_diagonal,
                                              approximate_score_target_position,
                                              z_drop, z_drop_gap_extend))
                {
                    break;
                }
            }
            if (anti_diagonal == query_length_i32 + target_length_i32 - 2 && unaligned_band_end == target_length_i32 - 1) {
                result.score = approx_score;
            }
        }
        previous_band_start = band_start;
        previous_band_end   = band_end;
    }

    // ====================================================================
    // Backtrack
    // ====================================================================
    if (should_compute_cigar) {
        const bool output_reversed = flags.reverse_cigar;
        if (!result.is_z_dropped && !flags.extz_only) {
            ksw_backtrack_cigar(backtrack, true, output_reversed, 0,
                                target_length_i32 - 1, query_length_i32 - 1, result.cigar);
        } else if (!result.is_z_dropped && flags.extz_only
                   && result.mqe + end_bonus > result.max)
        {
            result.has_reached_end = true;
            ksw_backtrack_cigar(backtrack, true, output_reversed, 0,
                                result.mqe_t, query_length_i32 - 1, result.cigar);
        } else if (result.max_t >= 0 && result.max_q >= 0) {
            ksw_backtrack_cigar(backtrack, true, output_reversed, 0,
                                result.max_t, result.max_q, result.cigar);
        }
    }

    return result;
}

// =============================================================================
// highway_ksw2_aligner — public API
// =============================================================================

/** Fixed-target facade for the single- and dual-affine KSW2 callables. */
class highway_ksw2_aligner {
public:
    /**
     * @brief Aligns with the single-affine recurrence.
     * @tparam p_is_reversed Whether query ranks use reverse orientation.
     * @tparam p_alphabet Logical sequence alphabet.
     * @param[in] query_sequence Query sequence facade.
     * @param[in] target_sequence Target sequence facade.
     * @param[in] query_start Inclusive query coordinate.
     * @param[in] query_end Exclusive query coordinate.
     * @param[in] target_start Inclusive target coordinate.
     * @param[in] target_end Exclusive target coordinate.
     * @param[in] scoring_matrix Canonical scalar and shuffle score matrix.
     * @param[in] parameters Single-affine KSW2 contract.
     * @return Alignment score, endpoints, termination state, and optional CIGAR.
     */
    template<
        bool p_is_reversed,
        sequence_alphabet p_alphabet>
    [[nodiscard]] static ksw_alignment_result align_single_affine(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const ksw_single_affine_parameters& parameters)
    {
        return align_ksw2_core<ksw_aligner_mode::single_affine, p_is_reversed>(
            query_sequence, target_sequence,
            query_start, query_end, target_start, target_end,
            scoring_matrix,
            parameters.gap_open, parameters.gap_extend,
            parameters.gap_open, parameters.gap_extend,
            parameters.band_width, parameters.z_drop,
            parameters.end_bonus, parameters.flags);
    }

    /**
     * @brief Aligns with the dual-affine recurrence.
     * @tparam p_is_reversed Whether query ranks use reverse orientation.
     * @tparam p_alphabet Logical sequence alphabet.
     * @param[in] query_sequence Query sequence facade.
     * @param[in] target_sequence Target sequence facade.
     * @param[in] query_start Inclusive query coordinate.
     * @param[in] query_end Exclusive query coordinate.
     * @param[in] target_start Inclusive target coordinate.
     * @param[in] target_end Exclusive target coordinate.
     * @param[in] scoring_matrix Canonical scalar and shuffle score matrix.
     * @param[in] parameters Dual-affine KSW2 contract.
     * @return Alignment score, endpoints, termination state, and optional CIGAR.
     */
    template<
        bool p_is_reversed,
        sequence_alphabet p_alphabet>
    [[nodiscard]] static ksw_alignment_result align_dual_affine(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        ksw_dual_affine_parameters parameters)
    {
        if (parameters.gap_open_long + parameters.gap_extend_long
            < parameters.gap_open_short + parameters.gap_extend_short)
        {
            std::swap(
                parameters.gap_open_short,
                parameters.gap_open_long);
            std::swap(
                parameters.gap_extend_short,
                parameters.gap_extend_long);
        }
        return align_ksw2_core<ksw_aligner_mode::dual_affine, p_is_reversed>(
            query_sequence, target_sequence,
            query_start, query_end, target_start, target_end,
            scoring_matrix,
            parameters.gap_open_short, parameters.gap_extend_short,
            parameters.gap_open_long, parameters.gap_extend_long,
            parameters.band_width, parameters.z_drop,
            parameters.end_bonus, parameters.flags);
    }
};

/**
 * @brief Dispatches the single-affine callable for the requested orientation.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param[in] is_reversed Whether query ranks use reverse orientation.
 * @param[in] query_sequence Query sequence facade.
 * @param[in] target_sequence Target sequence facade.
 * @param[in] query_start Inclusive query coordinate.
 * @param[in] query_end Exclusive query coordinate.
 * @param[in] target_start Inclusive target coordinate.
 * @param[in] target_end Exclusive target coordinate.
 * @param[in] scoring_matrix Canonical scalar and shuffle score matrix.
 * @param[in] parameters Single-affine KSW2 contract.
 * @return Alignment score, endpoints, termination state, and optional CIGAR.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result align_ksw2_single_affine(
    bool is_reversed,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const scoring_matrix<p_alphabet>& scoring_matrix,
    const ksw_single_affine_parameters& parameters)
{
    return is_reversed
        ? highway_ksw2_aligner::align_single_affine<true>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters)
        : highway_ksw2_aligner::align_single_affine<false>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters);
}

/**
 * @brief Dispatches the dual-affine callable for the requested orientation.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param[in] is_reversed Whether query ranks use reverse orientation.
 * @param[in] query_sequence Query sequence facade.
 * @param[in] target_sequence Target sequence facade.
 * @param[in] query_start Inclusive query coordinate.
 * @param[in] query_end Exclusive query coordinate.
 * @param[in] target_start Inclusive target coordinate.
 * @param[in] target_end Exclusive target coordinate.
 * @param[in] scoring_matrix Canonical scalar and shuffle score matrix.
 * @param[in] parameters Dual-affine KSW2 contract.
 * @return Alignment score, endpoints, termination state, and optional CIGAR.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result align_ksw2_dual_affine(
    bool is_reversed,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const scoring_matrix<p_alphabet>& scoring_matrix,
    ksw_dual_affine_parameters parameters)
{
    return is_reversed
        ? highway_ksw2_aligner::align_dual_affine<true>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters)
        : highway_ksw2_aligner::align_dual_affine<false>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters);
}

}  // namespace target
}  // namespace bioseq::simd
