/**
 * @file ksw2_kernel.cppm
 * @brief KSW2 alignment kernels for one CPU instruction set.
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
#include <concepts>
#include <type_traits>

export module bioseq.highway.sources.ksw2;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.common.checked_arithmetic;
import bioseq.highway.sources.alignment_utils;
import bioseq.highway.sources.ksw2_primitives;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

// KSW2 alignment with one or two affine gap-penalty pairs.

/**
 * Align two half-open sequence intervals with KSW2.
 *
 * SIMD lanes hold score differences along matrix  anti-diagonals, while wider
 * integers track accumulated scores and p_mode selects one gap-penalty pair
 * or the cheaper of two pairs for each gap. Temporary buffers are local.
 *
 * Query strand comes from p_is_reversed. Negative configured_band_width allows
 * the full matrix, and negative z_drop disables that stopping check. Output
 * flags select score-only results and CIGAR tie handling, and also control
 * end extension, where end_bonus favors reaching the query end.
 *
 * Return the score, endpoints, stopping state and optional owning CIGAR.
 * Throw std::length_error when coordinates or buffer sizes cannot be represented.
 */
template<
    ksw_aligner_mode p_mode,
    bool p_is_reversed,
    sequence_alphabet p_alphabet,
    size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u,
    class p_partial_score_type = ksw2_partial_score_type,
    class p_full_score_type = ksw2_full_score_type>
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

    // SIMD types for narrow  score differences and wider total scores.
    hn::ScalableTag<p_partial_score_type> simd_tag;
    hn::ScalableTag<p_full_score_type>    full_score_tag;

    const size_t byte_lane_count = hn::Lanes(simd_tag);
    const size_t score_lane_count = hn::Lanes(full_score_tag);
    constexpr size_t k_maximum_score_lanes = hn::MaxLanes(hn::ScalableTag<p_full_score_type>{});

    // Release DP buffers when this alignment finishes. The result owns its
    // CIGAR separately, so callers can keep it after the arena scope ends.
    auto align_scope = context::current().push<context::arena::alignment>();

    ksw_alignment_result result;

    if constexpr (p_alphabet_dimension <= 1) {
        return result;
    }

    const size_t query_length  = query_end - query_start;
    const size_t target_length = target_end - target_start;
    if (query_length == 0 || target_length == 0) return result;
    constexpr auto coordinate_limit =
        static_cast<size_t>((std::numeric_limits<int>::max)());
    const auto sequence_steps = bioseq::common::checked_add(query_length, target_length);
    if (!sequence_steps || query_length > coordinate_limit || target_length > coordinate_limit ||
        *sequence_steps - 1u > coordinate_limit) {
        throw std::length_error{"KSW2 storage exceeds the signed anti-diagonal domain"};
    }

    const int query_length_i32 = static_cast<int>(query_length);
    const int target_length_i32 = static_cast<int>(target_length);

    const int gap_open_plus_extend = gap_open + gap_extend;
    const bool should_compute_cigar = !flags.score_only;
    const bool should_use_approximate_maximum = flags.approx_max;

    // Reuse scoring constants across all SIMD updates.
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

    // A negative bandwidth allows every matrix cell.
    const int band_width = configured_band_width < 0
        ? std::max(target_length_i32, query_length_i32) : configured_band_width;
    const int left_band  = band_width;
    const int right_band = band_width;

    // Reject scores outside, the range supported by difference encoding.
    int minimum_matrix_score = scoring_matrix.mismatch_score();
    for (size_t score_index = 1; score_index < scoring_matrix.entries.size(); ++score_index) {
        if (scoring_matrix.entries[score_index] < minimum_matrix_score)
            minimum_matrix_score = scoring_matrix.entries[score_index];
    }
    if (-minimum_matrix_score > 2 * (gap_open + gap_extend)) return result;

    // Find the gap length where the long-gap penalty becomes cheaper.
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

    // Check buffer sizes before allocating the DP stte.
    const auto storage_plan = derive_ksw2_storage_plan(
        query_length, target_length, byte_lane_count, k_is_dual_affine,
        configured_band_width, !should_use_approximate_maximum,
        sizeof(p_full_score_type), should_compute_cigar);
    if (!storage_plan) {
        throw std::length_error{"KSW2 storage geometry is not representable"};
    }
    // Allocate the DP state in the alignment arena
    ksw_state_for<p_mode> state(*storage_plan);

    if constexpr (k_is_dual_affine) {
        state.fill_leading_arrays(4,
            static_cast<uint8_t>(-gap_open - gap_extend));
        state.fill_array(state.gap_e2_offset_base,
            static_cast<uint8_t>(-gap_open_long - gap_extend_long));
        state.fill_array(state.gap_f2_offset_base,
            static_cast<uint8_t>(-gap_open_long - gap_extend_long));
    }

    // Keep full scores for each anti-diagonal  when exact maximum tracking
    // is requested. This row is temporary alignment storage.
    p_full_score_type* exact_score_row = nullptr;
    if (!should_use_approximate_maximum) {
        const size_t exact_score_row_length = storage_plan->exact_score_elements;
        auto exact_row_span =
            context::current().alloc_align_array<p_full_score_type>(exact_score_row_length);
        std::fill(exact_row_span.begin(), exact_row_span.end(),
                  static_cast<p_full_score_type>(k_ksw_negative_infinity));
        exact_score_row = exact_row_span.data();
    }

    // Save traceback data only when CIGAR output is requested.
    ksw_backtrack_matrix backtrack;
    std::span<int32_t> anti_diagonal_band_starts{};
    std::span<int32_t> anti_diagonal_band_ends{};
    if (should_compute_cigar) {
        backtrack.allocate(*storage_plan);
        anti_diagonal_band_starts = backtrack.band_starts();
        anti_diagonal_band_ends   = backtrack.band_ends();
    }

    // Store the query backwards so corresponding symbols line up on each
    // anti-diagonal. For a left extension, reverse the target instead.
    // p_is_reversed independently selects the query strand.
    std::span<uint8_t> query_destination(state.query_reversed, query_length);
    std::span<uint8_t> target_destination(state.target_bytes, target_length);
    if (flags.reverse_cigar) {
        // Keep the oriented query in its current order for a left extension.
        query_seq.template copy_ranks<p_is_reversed, false>(
            query_destination, query_start, query_end);
        // Read the target backwards for the  left extension.
        target_seq.template copy_ranks<false, true>(
            target_destination, target_start, target_end);
    } else {
        // Reverse the oriented query for the anti-diagonal calculation.
        // On the reverse strand, the two reversals leave complemented bases.
        query_seq.template copy_ranks<p_is_reversed, true>(
            query_destination, query_start, query_end);
        target_seq.template copy_ranks<false, false>(
            target_destination, target_start, target_end);
    }

    // Track one promising position when approximate maxima are requested.
    p_full_score_type approx_score = 0;
    int approximate_score_target_position = 0;

    // Compare ranks inside the SIMD loop for the standard scoring matrix.
    const bool should_use_fused_score = !flags.generic_score;

    // Advance tmrough the scoring matrix one anti-diagonal at a time.
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

        // Supply boundary values for cells entering the active band.
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

        // Precompute symbol-pair scores for a custom, scoring  matrix.
        // Standard match, mismatch and wildcard scores are computed in the SIMD loop.
        if (flags.generic_score) {
            for (int target_position = unaligned_band_start; target_position <= unaligned_band_end; ++target_position) {
                state.diag_score_base[target_position] = scoring_matrix.at(
                    state.target_bytes[target_position], query_reversed_aligned[target_position]);
            }
        }

        // Put boundary values in the first vector lane.
        auto horizontal_gap_carry  = set_lane_zero(simd_tag, horizontal_gap_boundary_carry);
        auto horizontal_difference_carry  = set_lane_zero(simd_tag, horizontal_difference_boundary_carry);
        [[maybe_unused]] auto long_horizontal_gap_carry = set_lane_zero(simd_tag, long_horizontal_gap_boundary_carry);
        const int first_band_vector = band_start / byte_lane_count_i32;
        const int last_band_vector   = band_end / byte_lane_count_i32;

        // Choose score-only updates or traceback updates with the requested gap tie rule

        if (!should_compute_cigar) {
            // Update scores without saving traceback directions.
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
            // Save a CIGAR trace with left-aligned gaps.
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

                // Keep the earlier direction when scores tie.
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

                // Save score differences and gap state for the next anti-diagonal.
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
            // Save a CIGAR trace with right-aligned gaps.
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

                // Prefer the later gap direction when scores tie.
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

        // Track the best score and stop if the Z-drop limit is exceeded.
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
            // Follow  the better adjacent score to estimate the maximum.
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

    // Reconstruct the CIGAR from the selected endpoint.
    if (should_compute_cigar) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        result.cigar.set_logical_bounds(
            storage_plan->packed_cigar_step_bound,
            storage_plan->packed_cigar_capacity_bound);
#endif
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

// Entry points for the selected CPU instruction set.

/** KSW2 entry points with one or two gap-penalty pairs. */
class highway_ksw2_aligner {
public:
    /**
     * Align half-open query and target intervals with one gap-penalty pair.
     *
     * p_is_reversed selects the query strand. parameters sets the band, Z-drop,
     * end bonus and output flags. Return the score, endpoints and optional CIGAR.
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
     * Align half-opmn query and target intervals with two gap-penalty pairs.
     *
     * The cheaper pair determines the cost of each gap. This  wrapper puts the
     * pair with the cheaper one-symbol gap first before calling the core.
     * p_is_reversed selects the query strand, and parameters controls the band
     * and output flags. Return the score, endpoints and optional CIGAR.
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
 * Run single-affine KSW2 in the requested query orientation
 *
 * The query and, target intervals are half-open. parameters supplies the gap
 * penalties, band, Z-drop, end bonus and output flags. Return the alignment
 * score, endpoints, stopping state and optional CIGAR.
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
 * Run dual-affine KSW2 in the requested query orientation.
 *
 * The query and  target intervals are half-open. parameters supplies both
 * gap-penalty pairs, the band, Z-drop, end bonus and output flags. Return
 * the score, endpoints, stopping state and optional CIGAR.
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
