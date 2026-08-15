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

    const size_t query_len  = query_end - query_start;
    const size_t target_len = target_end - target_start;
    if (query_len == 0 || target_len == 0) return result;

    const int qlen = static_cast<int>(query_len);
    const int tlen = static_cast<int>(target_len);

    const int gap_open_plus_extend = gap_open + gap_extend;
    const bool compute_cigar = !flags.score_only;
    const bool use_approx_max = flags.approx_max;

    // --- SIMD constant vectors ---------------------------------------------
    const auto zero_vec          = hn::Zero(simd_tag);
    const auto match_score_vec   = hn::Set(simd_tag, scoring_matrix.match_score());
    const auto mismatch_score_vec = hn::Set(simd_tag, scoring_matrix.mismatch_score());
    const auto wildcard_rank_vec = hn::Set(simd_tag, static_cast<p_partial_score_type>(p_alphabet_dimension - 1));

    const auto double_gap_cost_vec = hn::Set(simd_tag,
        static_cast<p_partial_score_type>((gap_open + gap_extend) * 2));
    const auto gap_open_vec = hn::Set(simd_tag, gap_open);
    const auto max_plausible_score_vec = hn::Set(simd_tag,
        static_cast<p_partial_score_type>(scoring_matrix.match_score() + (gap_open + gap_extend) * 2));

    const auto gap_open_short_vec = hn::Set(simd_tag, gap_open);
    const auto gap_open_long_vec  = hn::Set(simd_tag, gap_open_long);
    const auto short_gap_cost_vec = hn::Set(simd_tag,
        static_cast<p_partial_score_type>(gap_open + gap_extend));
    const auto long_gap_cost_vec  = hn::Set(simd_tag,
        static_cast<p_partial_score_type>(gap_open_long + gap_extend_long));

    const int8_t wildcard_score = scoring_matrix.wildcard_score();
    auto wildcard_score_vec = hn::Set(
        simd_tag, static_cast<p_partial_score_type>(wildcard_score));
    if (wildcard_score == 0) {
        if constexpr (k_is_dual_affine) {
            wildcard_score_vec = hn::Set(
                simd_tag, static_cast<p_partial_score_type>(-gap_extend_long));
        } else {
            wildcard_score_vec = hn::Set(
                simd_tag, static_cast<p_partial_score_type>(-gap_extend));
        }
    }

    // Backtrack direction constants
    const auto bt_dir_1 = hn::Set(simd_tag, static_cast<p_partial_score_type>(1));
    const auto bt_dir_2 = hn::Set(simd_tag, static_cast<p_partial_score_type>(2));
    const auto bt_dir_3 = hn::Set(simd_tag, static_cast<p_partial_score_type>(3));
    const auto bt_dir_4 = hn::Set(simd_tag, static_cast<p_partial_score_type>(4));
    const auto bt_e_cont  = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x08));
    const auto bt_f_cont  = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x10));
    const auto bt_e2_cont = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x20));
    const auto bt_f2_cont = hn::Set(simd_tag, static_cast<p_partial_score_type>(0x40));

    // --- Band width --------------------------------------------------------
    const int band_width = configured_band_width < 0
        ? std::max(tlen, qlen) : configured_band_width;
    const int left_band  = band_width;
    const int right_band = band_width;

    // --- Score range sanity check ------------------------------------------
    int matrix_min = scoring_matrix.mismatch_score();
    for (size_t i = 1; i < scoring_matrix.entries.size(); ++i) {
        if (scoring_matrix.entries[i] < matrix_min)
            matrix_min = scoring_matrix.entries[i];
    }
    if (-matrix_min > 2 * (gap_open + gap_extend)) return result;

    // --- Dual-affine long-gap threshold ------------------------------------
    [[maybe_unused]] int long_gap_threshold = 0;
    [[maybe_unused]] int long_gap_diff = 0;
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
        long_gap_diff = long_gap_threshold * (gap_extend - gap_extend_long)
                        - (gap_open_long - gap_open) - gap_extend_long;
    }

    // --- Block dimensions --------------------------------------------------
    int band_vector_capacity = qlen < tlen ? qlen : tlen;
    band_vector_capacity = ((band_vector_capacity < band_width + 1
                            ? band_vector_capacity : band_width + 1)
                         + static_cast<int>(byte_lane_count) - 1)
                        / static_cast<int>(byte_lane_count) + 1;

    // --- State arena -------------------------------------------------------
    ksw_state_for<p_mode> state(target_len, query_len, byte_lane_count);

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
    if (!use_approx_max) {
        const size_t exact_row_len = static_cast<size_t>(state.target_vector_count()) * byte_lane_count;
        auto exact_row_span =
            context::current().alloc_align_array<p_full_score_type>(exact_row_len);
        std::fill(exact_row_span.begin(), exact_row_span.end(),
                  static_cast<p_full_score_type>(k_ksw_negative_infinity));
        exact_score_row = exact_row_span.data();
    }

    // --- Backtrack storage -------------------------------------------------
    ksw_backtrack_matrix backtrack;
    std::span<int32_t> anti_diag_band_start{};
    std::span<int32_t> anti_diag_band_end{};
    if (compute_cigar) {
        backtrack.allocate(qlen + tlen - 1, band_vector_capacity, byte_lane_count);
        anti_diag_band_start = backtrack.band_starts();
        anti_diag_band_end   = backtrack.band_ends();
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
    std::span<uint8_t> query_dst(state.query_reversed, query_len);
    std::span<uint8_t> target_dst(state.target_bytes, target_len);
    if (flags.reverse_cigar) {
        // qr stays in source order — no extra mirror.
        query_seq.template copy_ranks<p_is_reversed, false>(
            query_dst, query_start, query_end);
        // Target is reversed; no rc on target, so the second template arg is
        // the only orientation flip.
        target_seq.template copy_ranks<false, true>(
            target_dst, target_start, target_end);
    } else {
        // qr = reverse(encoded query). For a reversed strand the
        // existing <true, false> path produced rc(query); we want rc then
        // reverse-again, which is just complement-only. Captured as
        // <true, true> (see overload comment).
        query_seq.template copy_ranks<p_is_reversed, true>(
            query_dst, query_start, query_end);
        target_seq.template copy_ranks<false, false>(
            target_dst, target_start, target_end);
    }

    // --- Approximate max tracking ------------------------------------------
    p_full_score_type approx_score = 0;
    int approx_score_target_pos = 0;

    // --- Determine fusion eligibility (non-generic-score path) -------------
    const bool use_fused_score = !flags.generic_score;

    // ====================================================================
    // Main DP loop
    // ====================================================================
    const int bl = static_cast<int>(byte_lane_count);
    int prev_band_start = -1;
    int prev_band_end   = -1;

    for (int anti_diagonal = 0; anti_diagonal < qlen + tlen - 1; ++anti_diagonal) {
        int band_start = 0;
        int band_end   = tlen - 1;

        if (band_start < anti_diagonal - qlen + 1)
            band_start = anti_diagonal - qlen + 1;
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
        const int band_start_raw = band_start;
        const int band_end_raw   = band_end;
        band_start = (band_start / bl) * bl;
        band_end   = ((band_end + bl) / bl) * bl - 1;

        // --- Boundary conditions -------------------------------------------
        int8_t boundary_x_carry;
        int8_t boundary_v_carry;
        [[maybe_unused]] int8_t boundary_x2_carry = 0;
        uint8_t* HWY_RESTRICT query_reversed_aligned = state.query_reversed
            + (qlen - 1 - anti_diagonal);

        if constexpr (!k_is_dual_affine) {
            uint8_t* diff_up_u   = reinterpret_cast<uint8_t*>(state.diff_up_base);
            uint8_t* diff_left_u = reinterpret_cast<uint8_t*>(state.diff_left_base);
            uint8_t* gap_e_u     = reinterpret_cast<uint8_t*>(state.gap_e_offset_base);
            uint8_t* gap_f_u     = reinterpret_cast<uint8_t*>(state.gap_f_offset_base);

            if (band_start > 0) {
                if (band_start - 1 >= prev_band_start && band_start - 1 <= prev_band_end) {
                    boundary_x_carry = gap_e_u[band_start - 1];
                    boundary_v_carry = diff_left_u[band_start - 1];
                } else {
                    boundary_x_carry = 0;
                    boundary_v_carry = 0;
                }
            } else {
                boundary_x_carry = 0;
                boundary_v_carry = anti_diagonal ? static_cast<int8_t>(gap_open) : 0;
            }
            if (band_end >= anti_diagonal) {
                gap_f_u[anti_diagonal] = 0;
                diff_up_u[anti_diagonal] = anti_diagonal
                    ? static_cast<uint8_t>(gap_open) : 0;
            }
        } else {
            if (band_start > 0) {
                if (band_start - 1 >= prev_band_start && band_start - 1 <= prev_band_end) {
                    boundary_x_carry  = state.gap_e_offset_base[band_start - 1];
                    boundary_x2_carry = state.gap_e2_offset_base[band_start - 1];
                    boundary_v_carry  = state.diff_left_base[band_start - 1];
                } else {
                    boundary_x_carry  = static_cast<int8_t>(-gap_open - gap_extend);
                    boundary_x2_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
                    boundary_v_carry  = static_cast<int8_t>(-gap_open - gap_extend);
                }
            } else {
                boundary_x_carry  = static_cast<int8_t>(-gap_open - gap_extend);
                boundary_x2_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
                boundary_v_carry  = anti_diagonal == 0
                    ? static_cast<int8_t>(-gap_open - gap_extend)
                    : anti_diagonal < long_gap_threshold
                        ? static_cast<int8_t>(-gap_extend)
                        : anti_diagonal == long_gap_threshold
                            ? static_cast<int8_t>(long_gap_diff)
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
                            ? static_cast<int8_t>(long_gap_diff)
                            : static_cast<int8_t>(-gap_extend_long);
            }
        }

        // --- Compute per-cell scores (only for generic_score path) ---------
        // When !generic_score the scores are computed inline inside the DP
        // loop (diag_score fusion optimisation).
        if (flags.generic_score) {
            for (int tp = band_start_raw; tp <= band_end_raw; ++tp) {
                state.diag_score_base[tp] = scoring_matrix.at(
                    state.target_bytes[tp], query_reversed_aligned[tp]);
            }
        }

        // --- Boundary vectors ----------------------------------------------
        auto bx_vec  = set_lane_zero(simd_tag, boundary_x_carry);
        auto bv_vec  = set_lane_zero(simd_tag, boundary_v_carry);
        [[maybe_unused]] auto bx2_vec = set_lane_zero(simd_tag, boundary_x2_carry);
        const int band_start_vec = band_start / bl;
        const int band_end_vec   = band_end / bl;

        // ================================================================
        // Core DP inner loop — three paths depending on CIGAR / alignment
        // ================================================================

        if (!compute_cigar) {
            // ---- Score-only path ------------------------------------------
            for (int bvi = band_start_vec; bvi <= band_end_vec; ++bvi) {
                const int off = bvi * bl;
                auto c = use_fused_score
                    ? load_ksw_dp_cell<p_mode, true>(simd_tag, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec,
                        wildcard_rank_vec)
                    : load_ksw_dp_cell<p_mode, false>(simd_tag, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec,
                        wildcard_rank_vec);
                reduce_ksw_dp_maximum<p_mode>(simd_tag, c, max_plausible_score_vec,
                    match_score_vec);
                store_ksw_dp_cell<p_mode>(simd_tag, off, c, state,
                    gap_open_vec, gap_open_short_vec, gap_open_long_vec,
                    short_gap_cost_vec, long_gap_cost_vec, zero_vec);
            }
        } else if (!flags.right_align) {
            // ---- CIGAR, gap left-alignment ---------------------------------
            anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int bvi = band_start_vec; bvi <= band_end_vec; ++bvi) {
                const int off = bvi * bl;
                auto c = use_fused_score
                    ? load_ksw_dp_cell<p_mode, true>(simd_tag, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec,
                        wildcard_rank_vec)
                    : load_ksw_dp_cell<p_mode, false>(simd_tag, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec,
                        wildcard_rank_vec);

                // Direction tracking — left-align tie-break
                auto dir = hn::IfThenElseZero(
                    hn::Gt(c.diag_chain, c.score_max_cell), bt_dir_1);
                c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain);

                if constexpr (!k_is_dual_affine) {
                    dir = hn::IfThenElse(
                        hn::Gt(c.vert_chain, c.score_max_cell), bt_dir_2, dir);
                    c.score_max_cell = maximum_unsigned_bytes(simd_tag, c.score_max_cell, c.vert_chain);
                    c.score_max_cell = minimum_unsigned_bytes(simd_tag, c.score_max_cell, max_plausible_score_vec);
                } else {
                    dir = hn::IfThenElse(
                        hn::Gt(c.vert_chain, c.score_max_cell), bt_dir_2, dir);
                    c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain);
                    dir = hn::IfThenElse(
                        hn::Gt(c.diag_chain2, c.score_max_cell), bt_dir_3, dir);
                    c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain2);
                    dir = hn::IfThenElse(
                        hn::Gt(c.vert_chain2, c.score_max_cell), bt_dir_4, dir);
                    c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain2);
                    c.score_max_cell = hn::Min(c.score_max_cell, match_score_vec);
                }

                // Store diff + gap states
                hn::Store(hn::Sub(c.score_max_cell, c.diff_left_sh), simd_tag,
                          state.diff_up_base + off);
                hn::Store(hn::Sub(c.score_max_cell, c.diff_up_cur), simd_tag,
                          state.diff_left_base + off);

                if constexpr (!k_is_dual_affine) {
                    auto smq = hn::Sub(c.score_max_cell, gap_open_vec);
                    c.diag_chain = hn::Sub(c.diag_chain, smq);
                    c.vert_chain = hn::Sub(c.vert_chain, smq);

                    auto dp = hn::Gt(c.diag_chain, zero_vec);
                    hn::Store(hn::IfThenElseZero(dp, c.diag_chain), simd_tag,
                              state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(dp, bt_e_cont));

                    auto vp = hn::Gt(c.vert_chain, zero_vec);
                    hn::Store(hn::IfThenElseZero(vp, c.vert_chain), simd_tag,
                              state.gap_f_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(vp, bt_f_cont));
                } else {
                    auto sms = hn::Sub(c.score_max_cell, gap_open_short_vec);
                    c.diag_chain = hn::Sub(c.diag_chain, sms);
                    c.vert_chain = hn::Sub(c.vert_chain, sms);
                    auto sml = hn::Sub(c.score_max_cell, gap_open_long_vec);
                    c.diag_chain2 = hn::Sub(c.diag_chain2, sml);
                    c.vert_chain2 = hn::Sub(c.vert_chain2, sml);

                    auto dp = hn::Gt(c.diag_chain, zero_vec);
                    hn::Store(hn::Sub(hn::IfThenElseZero(dp, c.diag_chain),
                                       short_gap_cost_vec),
                              simd_tag, state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(dp, bt_e_cont));

                    auto vp = hn::Gt(c.vert_chain, zero_vec);
                    hn::Store(hn::Sub(hn::IfThenElseZero(vp, c.vert_chain),
                                       short_gap_cost_vec),
                              simd_tag, state.gap_f_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(vp, bt_f_cont));

                    auto d2p = hn::Gt(c.diag_chain2, zero_vec);
                    hn::Store(hn::Sub(hn::IfThenElseZero(d2p, c.diag_chain2),
                                       long_gap_cost_vec),
                              simd_tag, state.gap_e2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(d2p, bt_e2_cont));

                    auto v2p = hn::Gt(c.vert_chain2, zero_vec);
                    hn::Store(hn::Sub(hn::IfThenElseZero(v2p, c.vert_chain2),
                                       long_gap_cost_vec),
                              simd_tag, state.gap_f2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(v2p, bt_f2_cont));
                }

                hn::Store(dir, simd_tag,
                    backtrack.vector_slot_bytes(anti_diagonal, bvi - band_start_vec));
            }
        } else {
            // ---- CIGAR, gap right-alignment --------------------------------
            anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int bvi = band_start_vec; bvi <= band_end_vec; ++bvi) {
                const int off = bvi * bl;
                auto c = use_fused_score
                    ? load_ksw_dp_cell<p_mode, true>(simd_tag, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec,
                        wildcard_rank_vec)
                    : load_ksw_dp_cell<p_mode, false>(simd_tag, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec,
                        wildcard_rank_vec);

                // Direction tracking — right-align tie-break
                auto dir = hn::IfThenZeroElse(
                    hn::Gt(c.score_max_cell, c.diag_chain), bt_dir_1);
                c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain);

                if constexpr (!k_is_dual_affine) {
                    dir = hn::IfThenElse(
                        hn::Gt(c.score_max_cell, c.vert_chain), dir, bt_dir_2);
                    c.score_max_cell = maximum_unsigned_bytes(simd_tag, c.score_max_cell, c.vert_chain);
                    c.score_max_cell = minimum_unsigned_bytes(simd_tag, c.score_max_cell, max_plausible_score_vec);
                } else {
                    dir = hn::IfThenElse(
                        hn::Gt(c.score_max_cell, c.vert_chain), dir, bt_dir_2);
                    c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain);
                    dir = hn::IfThenElse(
                        hn::Gt(c.score_max_cell, c.diag_chain2), dir, bt_dir_3);
                    c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain2);
                    dir = hn::IfThenElse(
                        hn::Gt(c.score_max_cell, c.vert_chain2), dir, bt_dir_4);
                    c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain2);
                    c.score_max_cell = hn::Min(c.score_max_cell, match_score_vec);
                }

                hn::Store(hn::Sub(c.score_max_cell, c.diff_left_sh), simd_tag,
                          state.diff_up_base + off);
                hn::Store(hn::Sub(c.score_max_cell, c.diff_up_cur), simd_tag,
                          state.diff_left_base + off);

                if constexpr (!k_is_dual_affine) {
                    auto smq = hn::Sub(c.score_max_cell, gap_open_vec);
                    c.diag_chain = hn::Sub(c.diag_chain, smq);
                    c.vert_chain = hn::Sub(c.vert_chain, smq);

                    auto dn = hn::Gt(zero_vec, c.diag_chain);
                    hn::Store(hn::IfThenZeroElse(dn, c.diag_chain), simd_tag,
                              state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(dn, bt_e_cont));

                    auto vn = hn::Gt(zero_vec, c.vert_chain);
                    hn::Store(hn::IfThenZeroElse(vn, c.vert_chain), simd_tag,
                              state.gap_f_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(vn, bt_f_cont));
                } else {
                    auto sms = hn::Sub(c.score_max_cell, gap_open_short_vec);
                    c.diag_chain = hn::Sub(c.diag_chain, sms);
                    c.vert_chain = hn::Sub(c.vert_chain, sms);
                    auto sml = hn::Sub(c.score_max_cell, gap_open_long_vec);
                    c.diag_chain2 = hn::Sub(c.diag_chain2, sml);
                    c.vert_chain2 = hn::Sub(c.vert_chain2, sml);

                    auto dn = hn::Gt(zero_vec, c.diag_chain);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(dn, c.diag_chain),
                                       short_gap_cost_vec),
                              simd_tag, state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(dn, bt_e_cont));

                    auto vn = hn::Gt(zero_vec, c.vert_chain);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(vn, c.vert_chain),
                                       short_gap_cost_vec),
                              simd_tag, state.gap_f_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(vn, bt_f_cont));

                    auto d2n = hn::Gt(zero_vec, c.diag_chain2);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(d2n, c.diag_chain2),
                                       long_gap_cost_vec),
                              simd_tag, state.gap_e2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(d2n, bt_e2_cont));

                    auto v2n = hn::Gt(zero_vec, c.vert_chain2);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(v2n, c.vert_chain2),
                                       long_gap_cost_vec),
                              simd_tag, state.gap_f2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(v2n, bt_f2_cont));
                }

                hn::Store(dir, simd_tag,
                    backtrack.vector_slot_bytes(anti_diagonal, bvi - band_start_vec));
            }
        }

        // ================================================================
        // Max score tracking and Z-drop
        // ================================================================
        const int8_t zdrop_gap = k_is_dual_affine
            ? gap_extend_long
            : gap_extend;

        if (!use_approx_max) {
            p_full_score_type anti_diag_max_score;
            int anti_diag_max_target_pos;
            if (anti_diagonal > 0) {
                if constexpr (!k_is_dual_affine) {
                    const auto* du = reinterpret_cast<const uint8_t*>(state.diff_up_base);
                    const auto* dl = reinterpret_cast<const uint8_t*>(state.diff_left_base);
                    anti_diag_max_score = exact_score_row[band_end_raw] = band_end_raw > 0
                        ? exact_score_row[band_end_raw - 1]
                          + static_cast<p_full_score_type>(du[band_end_raw]) - gap_open_plus_extend
                        : exact_score_row[band_end_raw]
                          + static_cast<p_full_score_type>(dl[band_end_raw]) - gap_open_plus_extend;
                } else {
                    anti_diag_max_score = exact_score_row[band_end_raw] = band_end_raw > 0
                        ? exact_score_row[band_end_raw - 1]
                          + static_cast<p_full_score_type>(state.diff_up_base[band_end_raw])
                        : exact_score_row[band_end_raw]
                          + static_cast<p_full_score_type>(state.diff_left_base[band_end_raw]);
                }
                anti_diag_max_target_pos = band_end_raw;

                auto maximum_score_vector =
                    hn::Set(full_score_tag, anti_diag_max_score);
                auto maximum_position_vector = hn::Set(
                    full_score_tag,
                    static_cast<p_full_score_type>(
                        anti_diag_max_target_pos));
                [[maybe_unused]] auto gap_open_extend_vector = hn::Set(full_score_tag,
                    static_cast<p_full_score_type>(gap_open_plus_extend));

                const int block_end = band_start_raw
                    + (band_end_raw - band_start_raw) / static_cast<int>(score_lane_count)
                      * static_cast<int>(score_lane_count);
                int target_position = band_start_raw;
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
                    if (anti_diag_max_score < maximum_scores[lane_index]) {
                        anti_diag_max_score = maximum_scores[lane_index];
                        anti_diag_max_target_pos =
                            static_cast<int>(maximum_positions[lane_index])
                            + static_cast<int>(lane_index);
                    }
                }
                for (;
                     target_position < band_end_raw;
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
                        > anti_diag_max_score) {
                        anti_diag_max_score = exact_score_row[target_position];
                        anti_diag_max_target_pos = target_position;
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
                anti_diag_max_score = exact_score_row[0];
                anti_diag_max_target_pos = 0;
            }
            if (band_end_raw == tlen - 1 && exact_score_row[band_end_raw] > result.mte) {
                result.mte = exact_score_row[band_end_raw];
                result.mte_q = anti_diagonal - band_end_raw;
            }
            if (anti_diagonal - band_start_raw == qlen - 1
                && exact_score_row[band_start_raw] > result.mqe)
            {
                result.mqe = exact_score_row[band_start_raw];
                result.mqe_t = band_start_raw;
            }
            if (apply_z_drop_heuristic(result, true,
                                       anti_diag_max_score, anti_diagonal,
                                       anti_diag_max_target_pos,
                                       z_drop, zdrop_gap))
            {
                break;
            }
            if (anti_diagonal == qlen + tlen - 2 && band_end_raw == tlen - 1) {
                result.score = exact_score_row[tlen - 1];
            }
        } else {
            // --- Approximate max -------------------------------------------
            if (anti_diagonal > 0) {
                if (approx_score_target_pos >= band_start_raw
                    && approx_score_target_pos <= band_end_raw
                    && approx_score_target_pos + 1 >= band_start_raw
                    && approx_score_target_pos + 1 <= band_end_raw)
                {
                    p_full_score_type delta_left, delta_up;
                    if constexpr (!k_is_dual_affine) {
                        delta_left = static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[approx_score_target_pos])
                            - gap_open_plus_extend;
                        delta_up = static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_up_base)[approx_score_target_pos + 1])
                            - gap_open_plus_extend;
                    } else {
                        delta_left = state.diff_left_base[approx_score_target_pos];
                        delta_up   = state.diff_up_base[approx_score_target_pos + 1];
                    }
                    if (delta_left > delta_up) {
                        approx_score += delta_left;
                    } else {
                        approx_score += delta_up;
                        ++approx_score_target_pos;
                    }
                } else if (approx_score_target_pos >= band_start_raw
                           && approx_score_target_pos <= band_end_raw)
                {
                    if constexpr (!k_is_dual_affine) {
                        approx_score += static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[approx_score_target_pos])
                            - gap_open_plus_extend;
                    } else {
                        approx_score += static_cast<p_full_score_type>(
                            state.diff_left_base[approx_score_target_pos]);
                    }
                } else {
                    ++approx_score_target_pos;
                    if constexpr (!k_is_dual_affine) {
                        approx_score += static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_up_base)[approx_score_target_pos])
                            - gap_open_plus_extend;
                    } else {
                        approx_score += static_cast<p_full_score_type>(
                            state.diff_up_base[approx_score_target_pos]);
                    }
                }
                if constexpr (!k_is_dual_affine) {
                    if (flags.approx_zdrop
                        && apply_z_drop_heuristic(result, true,
                                                  approx_score, anti_diagonal,
                                                  approx_score_target_pos,
                                                  z_drop, zdrop_gap))
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
                approx_score_target_pos = 0;
            }
            if constexpr (k_is_dual_affine) {
                if (flags.approx_zdrop
                    && apply_z_drop_heuristic(result, true,
                                              approx_score, anti_diagonal,
                                              approx_score_target_pos,
                                              z_drop, zdrop_gap))
                {
                    break;
                }
            }
            if (anti_diagonal == qlen + tlen - 2 && band_end_raw == tlen - 1) {
                result.score = approx_score;
            }
        }
        prev_band_start = band_start;
        prev_band_end   = band_end;
    }

    // ====================================================================
    // Backtrack
    // ====================================================================
    if (compute_cigar) {
        const bool rev = flags.reverse_cigar;
        if (!result.is_z_dropped && !flags.extz_only) {
            ksw_backtrack_cigar(backtrack, true, rev, 0,
                                tlen - 1, qlen - 1, result.cigar);
        } else if (!result.is_z_dropped && flags.extz_only
                   && result.mqe + end_bonus > result.max)
        {
            result.has_reached_end = true;
            ksw_backtrack_cigar(backtrack, true, rev, 0,
                                result.mqe_t, qlen - 1, result.cigar);
        } else if (result.max_t >= 0 && result.max_q >= 0) {
            ksw_backtrack_cigar(backtrack, true, rev, 0,
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
