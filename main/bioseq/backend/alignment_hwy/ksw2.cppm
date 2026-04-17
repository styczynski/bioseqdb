/**
 * @file ksw2.cppm
 * @brief Highway (HWY) SIMD port of the KSW2 extension alignment algorithms.
 *
 * Portable SIMD implementations of the KSW2 extension alignment (Suzuki-
 * Kasahara diagonal formulation). Uses Google Highway so the code compiles
 * and runs on x86 (SSE2–AVX-512), ARM NEON/SVE, WASM, and any future target
 * Highway supports.
 *
 * Both single-affine-gap (z2) and dual-affine-gap (d2) are implemented by
 * a single unified kernel `ksw2_core_align<mode>`, templated on
 * `KswAlignerMode`. Public entry points live in `HwyKsw2Aligner`.
 *
 * Naming follows Suzuki's paper notation:
 *   - `anti_diagonal`       : `r` — anti-diagonal index, r = i + j
 *   - `target_position`     : `t` — target coordinate within an anti-diagonal
 *   - `diff_up[t]`          : `u` — H[i,j] - H[i-1,j] + q + e
 *   - `diff_left[t]`        : `v` — H[i,j] - H[i,j-1] + q + e
 *   - `gap_e_offset[t]`     : `x` — first gap state E
 *   - `gap_f_offset[t]`     : `y` — first gap state F
 *   - `gap_e2_offset[t]`    : `x2` (dual only) — long gap state E-tilde
 *   - `gap_f2_offset[t]`    : `y2` (dual only) — long gap state F-tilde
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <span>
#include <algorithm>
#include <type_traits>

export module alignment_hwy;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_cigar;
import alignment_hwy_utils;
import context;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// KswAlignerMode — selects single- or dual-affine-gap formulation.
// =============================================================================

enum class KswAlignerMode {
    SINGLE_AFFINE,  ///< z2: cost(k) = q + k*e
    DUAL_AFFINE,    ///< d2: cost(k) = min(q + k*e, q2 + k*e2)
};

// =============================================================================
// SIMD helpers — templated by tag D so no global type aliases are needed.
// =============================================================================

template<typename D, typename VecT = hn::Vec<D>>
[[maybe_unused]] [[always_inline]] inline VecT max_u8(
    const D& d, VecT left, VecT right) noexcept
{
    hn::RebindToUnsigned<D> ud;
    return hn::BitCast(d, hn::Max(hn::BitCast(ud, left), hn::BitCast(ud, right)));
}

template<typename D, typename VecT = hn::Vec<D>>
[[maybe_unused]] [[always_inline]] inline VecT min_u8(
    const D& d, VecT left, VecT right) noexcept
{
    hn::RebindToUnsigned<D> ud;
    return hn::BitCast(d, hn::Min(hn::BitCast(ud, left), hn::BitCast(ud, right)));
}

template<typename D, typename VecT = hn::Vec<D>>
[[always_inline]] inline VecT set_lane_zero(
    const D& d, hn::TFromD<D> value) noexcept
{
    constexpr size_t kMaxLanes = hn::MaxLanes(D{});
    HWY_ALIGN hn::TFromD<D> buffer[kMaxLanes] = {};
    buffer[0] = value;
    return hn::Load(d, buffer);
}

template<typename D, typename VecT = hn::Vec<D>>
[[always_inline]] inline VecT shift_left_insert(
    const D& d, VecT vec, VecT& lane0) noexcept
{
    constexpr size_t kElemBytes = sizeof(hn::TFromD<D>);
    constexpr size_t kVecBytes  = hn::MaxLanes(D{}) * kElemBytes;
    auto carry_out = hn::ShiftRightBytes<kVecBytes - kElemBytes>(d, vec);
    auto shifted   = hn::Or(hn::ShiftLeftBytes<kElemBytes>(d, vec), lane0);
    lane0 = carry_out;
    return shifted;
}

// =============================================================================
// Flags + parameter bundles
// =============================================================================

struct KswFlags {
    bool score_only    : 1 = false;
    bool right_align   : 1 = false;
    bool generic_score : 1 = false;
    bool approx_max    : 1 = false;
    bool approx_zdrop  : 1 = false;
    bool extz_only     : 1 = false;
    bool reverse_cigar : 1 = false;
    bool reserved      : 1 = false;
};
static_assert(sizeof(KswFlags) == 1);

struct KswZ2Params {
    int8_t    gap_open   = 0;
    int8_t    gap_extend = 0;
    int       band_width = -1;
    int       z_drop     = -1;
    int       end_bonus  = 0;
    KswFlags  flags      = {};
};

struct KswD2Params {
    int8_t    gap_open_short   = 0;
    int8_t    gap_extend_short = 0;
    int8_t    gap_open_long    = 0;
    int8_t    gap_extend_long  = 0;
    int       band_width       = -1;
    int       z_drop           = -1;
    int       end_bonus        = 0;
    KswFlags  flags            = {};
};

// =============================================================================
// KswAlignResult
// =============================================================================

constexpr int32_t kKswNegInf = -0x40000000;

struct KswAlignResult {
    int32_t max       = 0;
    int32_t score     = kKswNegInf;
    int32_t mqe       = kKswNegInf;
    int32_t mte       = kKswNegInf;
    int32_t max_q     = -1;
    int32_t max_t     = -1;
    int32_t mqe_t     = -1;
    int32_t mte_q     = -1;
    bool    zdropped  = false;
    bool    reach_end = false;
    std::vector<uint32_t> cigar;
};

// =============================================================================
// Z-drop heuristic
// =============================================================================

[[nodiscard]] inline bool apply_zdrop_heuristic(
    KswAlignResult& result,
    bool use_rotated_coords,
    int32_t current_score,
    int first_coord,
    int second_coord,
    int zdrop_threshold,
    int8_t gap_extension_penalty) noexcept
{
    int anti_diagonal;
    int target_position;
    if (use_rotated_coords) {
        anti_diagonal   = first_coord;
        target_position = second_coord;
    } else {
        anti_diagonal   = first_coord + second_coord;
        target_position = first_coord;
    }
    if (current_score > result.max) {
        result.max   = current_score;
        result.max_t = target_position;
        result.max_q = anti_diagonal - target_position;
        return false;
    }
    if (target_position >= result.max_t
        && anti_diagonal - target_position >= result.max_q)
    {
        const int td = target_position - result.max_t;
        const int qd = (anti_diagonal - target_position) - result.max_q;
        const int cd = td > qd ? td - qd : qd - td;
        if (zdrop_threshold >= 0
            && result.max - current_score
               > zdrop_threshold + cd * gap_extension_penalty)
        {
            result.zdropped = true;
            return true;
        }
    }
    return false;
}

// =============================================================================
// KswDpState — named-field DP arena.
// =============================================================================

class KswDpState {
public:
    int8_t*  diff_up_base       = nullptr;
    int8_t*  diff_left_base     = nullptr;
    int8_t*  gap_e_offset_base  = nullptr;
    int8_t*  gap_f_offset_base  = nullptr;
    int8_t*  diag_score_base    = nullptr;
    uint8_t* target_bytes       = nullptr;
    uint8_t* query_reversed     = nullptr;

    KswDpState() = default;

    KswDpState(size_t target_len, size_t query_len, size_t vec_bytes)
        : KswDpState(target_len, query_len, vec_bytes, 5) {}

    [[nodiscard]] int target_vec_count() const noexcept { return target_vec_count_; }
    [[nodiscard]] int query_vec_count()  const noexcept { return query_vec_count_; }
    [[nodiscard]] size_t array_byte_count() const noexcept {
        return static_cast<size_t>(target_vec_count_) * vec_bytes_;
    }

    void fill_first_n_arrays(int n, uint8_t value) noexcept {
        std::memset(diff_up_base, value, static_cast<size_t>(n) * array_byte_count());
    }

    void fill_array(int8_t* base, uint8_t value) noexcept {
        std::memset(base, value, array_byte_count());
    }

protected:
    // Storage comes from the per-phase align arena (SPEC §5.2). We only
    // zero the DP-state prefix of the slab; target_bytes / query_reversed
    // are immediately overwritten by the encode step, so skipping the
    // zero-fill there (SPEC §6.44) saves ~20 KiB of writes per region.
    KswDpState(size_t target_len, size_t query_len, size_t vec_bytes, int state_count)
        : target_vec_count_(static_cast<int>((target_len + vec_bytes - 1) / vec_bytes))
        , query_vec_count_(static_cast<int>((query_len + vec_bytes - 1) / vec_bytes))
        , vec_bytes_(vec_bytes)
    {
        const size_t stride = static_cast<size_t>(target_vec_count_) * vec_bytes;
        const size_t total  = (static_cast<size_t>(state_count + 1) * target_vec_count_
                              + static_cast<size_t>(query_vec_count_) + 1) * vec_bytes;
        storage_ = context::current().alloc_align_array<int8_t, 64>(total);
        const size_t dp_bytes = static_cast<size_t>(state_count) * stride;
        std::memset(storage_.data(), 0, dp_bytes);
        // Wipe the single extra byte that diag_score_base writes read just
        // past the DP-state arrays to keep the boundary deterministic.
        if (storage_.size() > dp_bytes) {
            storage_[dp_bytes] = 0;
        }

        int8_t* base = storage_.data();
        diff_up_base      = base;
        diff_left_base    = diff_up_base      + stride;
        gap_e_offset_base = diff_left_base    + stride;
        gap_f_offset_base = gap_e_offset_base + stride;
        diag_score_base   = base + static_cast<size_t>(state_count - 1) * stride;
        target_bytes      = reinterpret_cast<uint8_t*>(
            base + static_cast<size_t>(state_count) * stride);
        query_reversed    = reinterpret_cast<uint8_t*>(
            base + static_cast<size_t>(state_count + 1) * stride);
    }

    [[nodiscard]] int8_t* array_ptr(int index) noexcept {
        return storage_.data()
            + static_cast<size_t>(index) * target_vec_count_ * vec_bytes_;
    }

    int target_vec_count_ = 0;
    int query_vec_count_  = 0;
    size_t vec_bytes_     = 0;
    std::span<int8_t> storage_;
};

class KswDpStateDual : public KswDpState {
public:
    int8_t* gap_e2_offset_base = nullptr;
    int8_t* gap_f2_offset_base = nullptr;

    KswDpStateDual() = default;

    KswDpStateDual(size_t target_len, size_t query_len, size_t vec_bytes)
        : KswDpState(target_len, query_len, vec_bytes, 7)
    {
        gap_e2_offset_base = array_ptr(4);
        gap_f2_offset_base = array_ptr(5);
    }
};

template<KswAlignerMode mode>
using KswStateFor = std::conditional_t<
    mode == KswAlignerMode::DUAL_AFFINE,
    KswDpStateDual,
    KswDpState>;

// =============================================================================
// KswBacktrackMatrix
// =============================================================================

class KswBacktrackMatrix {
public:
    KswBacktrackMatrix() noexcept = default;

    // Storage and band metadata all come from the align arena (SPEC §5.2).
    // The DP forward pass writes every in-band slot before traceback reads
    // it, so we skip the old `memset(0, total_bytes)` (SPEC §6.52) — for
    // a 10 kbp × 10 kbp alignment that saves ~15 MiB of writes per phase.
    void allocate(int anti_diagonal_count, int band_vec_capacity, size_t vec_bytes) {
        vec_bytes_ = vec_bytes;
        cols_in_vectors_ = band_vec_capacity;
        byte_cols_per_row_ = band_vec_capacity * static_cast<int>(vec_bytes);
        total_vector_slots_ =
            static_cast<size_t>(anti_diagonal_count) * cols_in_vectors_ + 1;
        const size_t total_bytes = total_vector_slots_ * vec_bytes;
        auto& ctx = context::current();
        storage_ = ctx.alloc_align_array<int8_t, 64>(total_bytes);
        band_range_ = ctx.alloc_align_array<int32_t>(
            2 * static_cast<size_t>(anti_diagonal_count));
    }

    [[nodiscard]] bool is_allocated() const noexcept { return !storage_.empty(); }

    [[nodiscard]] int8_t* vec_slot_bytes(int anti_diagonal, int local_band_vec) noexcept {
        const size_t slot =
            static_cast<size_t>(anti_diagonal) * cols_in_vectors_
            + static_cast<size_t>(local_band_vec);
        return storage_.data() + slot * vec_bytes_;
    }

    [[nodiscard]] const uint8_t* bytes() const noexcept {
        return reinterpret_cast<const uint8_t*>(storage_.data());
    }
    [[nodiscard]] int byte_cols_per_row() const noexcept { return byte_cols_per_row_; }
    [[nodiscard]] std::span<int32_t> band_starts() noexcept {
        return band_range_.subspan(0, band_range_.size() / 2);
    }
    [[nodiscard]] std::span<int32_t> band_ends() noexcept {
        return band_range_.subspan(band_range_.size() / 2);
    }
    [[nodiscard]] std::span<const int32_t> band_starts() const noexcept {
        return std::span<const int32_t>(band_range_.data(), band_range_.size() / 2);
    }
    [[nodiscard]] std::span<const int32_t> band_ends() const noexcept {
        return std::span<const int32_t>(band_range_.data() + band_range_.size() / 2,
                                        band_range_.size() / 2);
    }

private:
    std::span<int8_t>  storage_;
    std::span<int32_t> band_range_;
    size_t vec_bytes_          = 0;
    int    cols_in_vectors_    = 0;
    int    byte_cols_per_row_  = 0;
    size_t total_vector_slots_ = 0;
};

// =============================================================================
// CIGAR traceback
// =============================================================================

inline void ksw_backtrack_cigar(
    const KswBacktrackMatrix& matrix,
    bool use_rotated_coords,
    bool output_reversed,
    int min_intron_length,
    int start_row,
    int start_col,
    std::vector<uint32_t>& out_cigar)
{
    out_cigar.clear();

    const uint8_t* bt = matrix.bytes();
    const std::span<const int32_t> off = matrix.band_starts();
    const std::span<const int32_t> off_end = matrix.band_ends();
    const int n_col = matrix.byte_cols_per_row();

    int i = start_row, k = start_col, state = 0;
    while (i >= 0 && k >= 0) {
        int force = -1;
        uint32_t bv = 0;
        if (use_rotated_coords) {
            const int r = i + k;
            if (i < off[static_cast<size_t>(r)]) force = 2;
            if (i > off_end[static_cast<size_t>(r)]) force = 1;
            if (force < 0)
                bv = bt[static_cast<size_t>(r) * n_col
                        + static_cast<size_t>(i - off[static_cast<size_t>(r)])];
        } else {
            if (k < off[static_cast<size_t>(i)]) force = 2;
            if (k > off_end[static_cast<size_t>(i)]) force = 1;
            if (force < 0)
                bv = bt[static_cast<size_t>(i) * n_col
                        + static_cast<size_t>(k - off[static_cast<size_t>(i)])];
        }

        if (state == 0) state = static_cast<int>(bv & 7U);
        else if (!((bv >> (state + 2)) & 1U)) state = 0;
        if (state == 0) state = static_cast<int>(bv & 7U);
        if (force >= 0) state = force;

        if (state == 0)      { push_packed_cigar(out_cigar, CIGAR_MATCH, 1); --i; --k; }
        else if (state == 1 || (state == 3 && min_intron_length <= 0))
                             { push_packed_cigar(out_cigar, CIGAR_DEL, 1); --i; }
        else if (state == 3 && min_intron_length > 0)
                             { push_packed_cigar(out_cigar, CIGAR_N_SKIP, 1); --i; }
        else                 { push_packed_cigar(out_cigar, CIGAR_INS, 1); --k; }
    }

    if (i >= 0) {
        const uint32_t op = (min_intron_length > 0 && i >= min_intron_length)
            ? CIGAR_N_SKIP : CIGAR_DEL;
        push_packed_cigar(out_cigar, op, static_cast<uint32_t>(i + 1));
    }
    if (k >= 0) push_packed_cigar(out_cigar, CIGAR_INS, static_cast<uint32_t>(k + 1));
    if (!output_reversed) std::reverse(out_cigar.begin(), out_cigar.end());
}

// =============================================================================
// KswDpCell — return type for the DP load step (replaces KSW_DP_LOAD macro).
// =============================================================================

template<typename D>
struct KswDpCell {
    hn::Vec<D> score_max_cell;
    hn::Vec<D> diff_left_sh;
    hn::Vec<D> diff_up_cur;
    hn::Vec<D> diag_chain;
    hn::Vec<D> vert_chain;
    hn::Vec<D> diag_chain2;
    hn::Vec<D> vert_chain2;
};

// =============================================================================
// ksw_dp_load — template replacement for the KSW_DP_LOAD macro.
//
// When fused_score == true the cell score is computed inline from sequence
// data (the diag_score fusion optimisation from CAN_WE_REMOVE_LOAD_STORE.md).
// When false the score is loaded from diag_score_base (generic_score path).
// =============================================================================

template<KswAlignerMode mode, bool fused_score, typename D, typename StateT>
[[always_inline]] inline KswDpCell<D> ksw_dp_load(
    D pst, int off,
    StateT& HWY_RESTRICT state,
    const uint8_t* HWY_RESTRICT query_reversed_aligned,
    hn::Vec<D>& bx_vec, hn::Vec<D>& bv_vec,
    [[maybe_unused]] hn::Vec<D>& bx2_vec,
    hn::Vec<D> double_gap_cost_vec,
    hn::Vec<D> match_score_vec,
    hn::Vec<D> mismatch_score_vec,
    hn::Vec<D> wildcard_score_vec,
    hn::Vec<D> wildcard_rank_vec) noexcept
{
    constexpr bool is_dual = (mode == KswAlignerMode::DUAL_AFFINE);
    using partial_t = hn::TFromD<D>;

    KswDpCell<D> c;

    // --- Cell score ---
    if constexpr (fused_score) {
        auto tl = hn::LoadU(pst,
            reinterpret_cast<const partial_t*>(&state.target_bytes[off]));
        auto ql = hn::LoadU(pst,
            reinterpret_cast<const partial_t*>(&query_reversed_aligned[off]));
        auto wm = hn::Or(
            hn::VecFromMask(pst, hn::Eq(tl, wildcard_rank_vec)),
            hn::VecFromMask(pst, hn::Eq(ql, wildcard_rank_vec)));
        auto eq = hn::VecFromMask(pst, hn::Eq(tl, ql));
        c.score_max_cell = hn::IfThenElse(
            hn::MaskFromVec(eq), match_score_vec, mismatch_score_vec);
        c.score_max_cell = hn::IfThenElse(
            hn::MaskFromVec(wm), wildcard_score_vec, c.score_max_cell);
    } else {
        c.score_max_cell = hn::Load(pst, state.diag_score_base + off);
    }

    if constexpr (!is_dual) {
        c.score_max_cell = hn::Add(c.score_max_cell, double_gap_cost_vec);
    }

    // --- Gap / diff state ---
    auto gap_e_sh = hn::Load(pst, state.gap_e_offset_base + off);
    gap_e_sh = shift_left_insert(pst, gap_e_sh, bx_vec);
    c.diff_left_sh = hn::Load(pst, state.diff_left_base + off);
    c.diff_left_sh = shift_left_insert(pst, c.diff_left_sh, bv_vec);
    c.diag_chain = hn::Add(gap_e_sh, c.diff_left_sh);
    c.diff_up_cur = hn::Load(pst, state.diff_up_base + off);
    c.vert_chain = hn::Add(
        hn::Load(pst, state.gap_f_offset_base + off), c.diff_up_cur);

    c.diag_chain2 = hn::Zero(pst);
    c.vert_chain2 = hn::Zero(pst);
    if constexpr (is_dual) {
        auto ge2_sh = hn::Load(pst, state.gap_e2_offset_base + off);
        ge2_sh = shift_left_insert(pst, ge2_sh, bx2_vec);
        c.diag_chain2 = hn::Add(ge2_sh, c.diff_left_sh);
        c.vert_chain2 = hn::Add(
            hn::Load(pst, state.gap_f2_offset_base + off), c.diff_up_cur);
    }

    return c;
}

// =============================================================================
// ksw_dp_max_reduce — template replacement for KSW_DP_MAX_REDUCE.
// =============================================================================

template<KswAlignerMode mode, typename D>
[[always_inline]] inline void ksw_dp_max_reduce(
    D pst, KswDpCell<D>& c,
    hn::Vec<D> max_plausible_score_vec,
    hn::Vec<D> match_score_vec) noexcept
{
    constexpr bool is_dual = (mode == KswAlignerMode::DUAL_AFFINE);

    c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain);
    if constexpr (!is_dual) {
        c.score_max_cell = max_u8(pst, c.score_max_cell, c.vert_chain);
        c.score_max_cell = min_u8(pst, c.score_max_cell, max_plausible_score_vec);
    } else {
        c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain);
        c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain2);
        c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain2);
        c.score_max_cell = hn::Min(c.score_max_cell, match_score_vec);
    }
}

// =============================================================================
// ksw_dp_store — template replacement for KSW_DP_UPDATE.
// =============================================================================

template<KswAlignerMode mode, typename D, typename StateT>
[[always_inline]] inline void ksw_dp_store(
    D pst, int off,
    KswDpCell<D>& c,
    StateT& HWY_RESTRICT state,
    hn::Vec<D> gap_open_vec,
    hn::Vec<D> gap_open_short_vec,
    hn::Vec<D> gap_open_long_vec,
    hn::Vec<D> short_gap_cost_vec,
    hn::Vec<D> long_gap_cost_vec,
    hn::Vec<D> zero_vec) noexcept
{
    constexpr bool is_dual = (mode == KswAlignerMode::DUAL_AFFINE);

    hn::Store(hn::Sub(c.score_max_cell, c.diff_left_sh), pst,
              state.diff_up_base + off);
    hn::Store(hn::Sub(c.score_max_cell, c.diff_up_cur), pst,
              state.diff_left_base + off);

    if constexpr (!is_dual) {
        auto smq = hn::Sub(c.score_max_cell, gap_open_vec);
        c.diag_chain = hn::Sub(c.diag_chain, smq);
        c.vert_chain = hn::Sub(c.vert_chain, smq);
        hn::Store(hn::Max(c.diag_chain, zero_vec), pst,
                  state.gap_e_offset_base + off);
        hn::Store(hn::Max(c.vert_chain, zero_vec), pst,
                  state.gap_f_offset_base + off);
    } else {
        auto sms = hn::Sub(c.score_max_cell, gap_open_short_vec);
        c.diag_chain = hn::Sub(c.diag_chain, sms);
        c.vert_chain = hn::Sub(c.vert_chain, sms);
        auto sml = hn::Sub(c.score_max_cell, gap_open_long_vec);
        c.diag_chain2 = hn::Sub(c.diag_chain2, sml);
        c.vert_chain2 = hn::Sub(c.vert_chain2, sml);
        hn::Store(hn::Sub(hn::Max(c.diag_chain, zero_vec), short_gap_cost_vec),
                  pst, state.gap_e_offset_base + off);
        hn::Store(hn::Sub(hn::Max(c.vert_chain, zero_vec), short_gap_cost_vec),
                  pst, state.gap_f_offset_base + off);
        hn::Store(hn::Sub(hn::Max(c.diag_chain2, zero_vec), long_gap_cost_vec),
                  pst, state.gap_e2_offset_base + off);
        hn::Store(hn::Sub(hn::Max(c.vert_chain2, zero_vec), long_gap_cost_vec),
                  pst, state.gap_f2_offset_base + off);
    }
}

// =============================================================================
// ksw2_core_align — unified single/dual-affine banded extension alignment
// =============================================================================

template<
    KswAlignerMode mode,
    bool is_reversed,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange,
    size_t AlphabetDim = compact_sequence_range<QueryRange>::traits::alphabet_size_n,
    typename partial_score_t = int8_t,
    typename full_score_t    = int32_t>
[[nodiscard]] KswAlignResult ksw2_core_align(
    const compact_sequence_range<QueryRange>& query_seq,
    const compact_sequence_range<TargetRange>& target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    int8_t gap_open,
    int8_t gap_extend,
    int8_t gap_open_long,
    int8_t gap_extend_long,
    int band_width_param,
    int z_drop,
    int end_bonus,
    KswFlags flags)
{
    constexpr bool is_dual = (mode == KswAlignerMode::DUAL_AFFINE);

    // --- SIMD tags ---------------------------------------------------------
    hn::ScalableTag<partial_score_t> pst;
    hn::ScalableTag<full_score_t>    fst;

    const size_t byte_lanes = hn::Lanes(pst);
    constexpr size_t byte_lanes_max = hn::MaxLanes(hn::ScalableTag<partial_score_t>{});
    const size_t score_lanes = hn::Lanes(fst);
    constexpr size_t score_lanes_max = hn::MaxLanes(hn::ScalableTag<full_score_t>{});

    // Every internal buffer (DP state, backtrack matrix, exact-score row,
    // encoded sequences) lives in this scope. The CIGAR output lives on
    // the result struct's own vector (heap) and is built BEFORE the scope
    // unwinds, so the dangling-span hazard is contained here.
    auto align_scope = context::current().push<context::Arena::Align>();

    KswAlignResult result;

    if constexpr (AlphabetDim <= 1) {
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
    const auto zero_vec          = hn::Zero(pst);
    const auto match_score_vec   = hn::Set(pst, scoring_matrix.match_score());
    const auto mismatch_score_vec = hn::Set(pst, scoring_matrix.mismatch_score());
    const auto wildcard_rank_vec = hn::Set(pst, static_cast<partial_score_t>(AlphabetDim - 1));

    const auto double_gap_cost_vec = hn::Set(pst,
        static_cast<partial_score_t>((gap_open + gap_extend) * 2));
    const auto gap_open_vec = hn::Set(pst, gap_open);
    const auto max_plausible_score_vec = hn::Set(pst,
        static_cast<partial_score_t>(scoring_matrix.match_score() + (gap_open + gap_extend) * 2));

    const auto gap_open_short_vec = hn::Set(pst, gap_open);
    const auto gap_open_long_vec  = hn::Set(pst, gap_open_long);
    const auto short_gap_cost_vec = hn::Set(pst,
        static_cast<partial_score_t>(gap_open + gap_extend));
    const auto long_gap_cost_vec  = hn::Set(pst,
        static_cast<partial_score_t>(gap_open_long + gap_extend_long));

    const auto wildcard_score_vec = [&]() {
        const int8_t wc = scoring_matrix.wildcard_score();
        if (wc == 0) {
            if constexpr (is_dual)
                return hn::Set(pst, static_cast<partial_score_t>(-gap_extend_long));
            else
                return hn::Set(pst, static_cast<partial_score_t>(-gap_extend));
        }
        return hn::Set(pst, static_cast<partial_score_t>(wc));
    }();

    // Backtrack direction constants
    const auto bt_dir_1 = hn::Set(pst, static_cast<partial_score_t>(1));
    const auto bt_dir_2 = hn::Set(pst, static_cast<partial_score_t>(2));
    const auto bt_dir_3 = hn::Set(pst, static_cast<partial_score_t>(3));
    const auto bt_dir_4 = hn::Set(pst, static_cast<partial_score_t>(4));
    const auto bt_e_cont  = hn::Set(pst, static_cast<partial_score_t>(0x08));
    const auto bt_f_cont  = hn::Set(pst, static_cast<partial_score_t>(0x10));
    const auto bt_e2_cont = hn::Set(pst, static_cast<partial_score_t>(0x20));
    const auto bt_f2_cont = hn::Set(pst, static_cast<partial_score_t>(0x40));

    // --- Band width --------------------------------------------------------
    const int band_width = band_width_param < 0
        ? std::max(tlen, qlen) : band_width_param;
    const int left_band  = band_width;
    const int right_band = band_width;

    // --- Score range sanity check ------------------------------------------
    int matrix_min = scoring_matrix.mismatch_score();
    for (size_t i = 1; i < scoring_matrix.entry_count; ++i) {
        if (scoring_matrix.entries[i] < matrix_min)
            matrix_min = scoring_matrix.entries[i];
    }
    if (-matrix_min > 2 * (gap_open + gap_extend)) return result;

    // --- Long-gap threshold (d2 only) --------------------------------------
    [[maybe_unused]] int long_gap_threshold = 0;
    [[maybe_unused]] int long_gap_diff = 0;
    if constexpr (is_dual) {
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
    int band_vec_capacity = qlen < tlen ? qlen : tlen;
    band_vec_capacity = ((band_vec_capacity < band_width + 1
                            ? band_vec_capacity : band_width + 1)
                         + static_cast<int>(byte_lanes_max) - 1)
                        / static_cast<int>(byte_lanes_max) + 1;

    // --- State arena -------------------------------------------------------
    KswStateFor<mode> state(target_len, query_len, byte_lanes);

    if constexpr (is_dual) {
        state.fill_first_n_arrays(4,
            static_cast<uint8_t>(-gap_open - gap_extend));
        state.fill_array(state.gap_e2_offset_base,
            static_cast<uint8_t>(-gap_open_long - gap_extend_long));
        state.fill_array(state.gap_f2_offset_base,
            static_cast<uint8_t>(-gap_open_long - gap_extend_long));
    }

    // --- Exact max tracking ------------------------------------------------
    // Exact-score row goes in the align arena; it's re-read every anti-
    // diagonal but never survives the call.
    full_score_t* exact_score_row = nullptr;
    if (!use_approx_max) {
        const size_t exact_row_len = static_cast<size_t>(state.target_vec_count()) * byte_lanes;
        auto exact_row_span =
            context::current().alloc_align_array<full_score_t>(exact_row_len);
        std::fill(exact_row_span.begin(), exact_row_span.end(),
                  static_cast<full_score_t>(kKswNegInf));
        exact_score_row = exact_row_span.data();
    }

    // --- Backtrack storage -------------------------------------------------
    KswBacktrackMatrix backtrack;
    std::span<int32_t> anti_diag_band_start{};
    std::span<int32_t> anti_diag_band_end{};
    if (compute_cigar) {
        backtrack.allocate(qlen + tlen - 1, band_vec_capacity, byte_lanes);
        anti_diag_band_start = backtrack.band_starts();
        anti_diag_band_end   = backtrack.band_ends();
    }

    // --- Prepare sequences -------------------------------------------------
    // Encode directly into caller-provided spans (SPEC §6.7). The
    // scratch query/target buffers are short-lived, so they live in the
    // align arena alongside the DP state, avoiding two system mallocs
    // per phase.
    auto& encode_ctx = context::current();
    auto query_enc  = encode_ctx.alloc_align_array<uint8_t>(query_len);
    auto target_enc = encode_ctx.alloc_align_array<uint8_t>(target_len);
    query_seq.template to_encoded_into<is_reversed>(query_enc, query_start, query_end);
    target_seq.template to_encoded_into<false>(target_enc, target_start, target_end);

    if (flags.reverse_cigar) {
        std::memcpy(state.query_reversed, query_enc.data(), query_len);
        for (size_t i = 0; i < target_len; ++i)
            state.target_bytes[i] = target_enc[target_len - 1 - i];
    } else {
        for (size_t i = 0; i < query_len; ++i)
            state.query_reversed[i] = query_enc[query_len - 1 - i];
        std::memcpy(state.target_bytes, target_enc.data(), target_len);
    }

    // --- Approximate max tracking ------------------------------------------
    full_score_t approx_score = 0;
    int approx_score_target_pos = 0;

    // --- Determine fusion eligibility (non-generic-score path) -------------
    const bool use_fused_score = !flags.generic_score;

    // ====================================================================
    // Main DP loop
    // ====================================================================
    const int bl = static_cast<int>(byte_lanes);
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
            result.zdropped = true;
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

        if constexpr (!is_dual) {
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
        auto bx_vec  = set_lane_zero(pst, boundary_x_carry);
        auto bv_vec  = set_lane_zero(pst, boundary_v_carry);
        [[maybe_unused]] auto bx2_vec = set_lane_zero(pst, boundary_x2_carry);
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
                    ? ksw_dp_load<mode, true>(pst, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec, wildcard_rank_vec)
                    : ksw_dp_load<mode, false>(pst, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec, wildcard_rank_vec);
                ksw_dp_max_reduce<mode>(pst, c, max_plausible_score_vec,
                    match_score_vec);
                ksw_dp_store<mode>(pst, off, c, state,
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
                    ? ksw_dp_load<mode, true>(pst, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec, wildcard_rank_vec)
                    : ksw_dp_load<mode, false>(pst, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec, wildcard_rank_vec);

                // Direction tracking — left-align tie-break
                auto dir = hn::IfThenElseZero(
                    hn::Gt(c.diag_chain, c.score_max_cell), bt_dir_1);
                c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain);

                if constexpr (!is_dual) {
                    dir = hn::IfThenElse(
                        hn::Gt(c.vert_chain, c.score_max_cell), bt_dir_2, dir);
                    c.score_max_cell = max_u8(pst, c.score_max_cell, c.vert_chain);
                    c.score_max_cell = min_u8(pst, c.score_max_cell, max_plausible_score_vec);
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
                hn::Store(hn::Sub(c.score_max_cell, c.diff_left_sh), pst,
                          state.diff_up_base + off);
                hn::Store(hn::Sub(c.score_max_cell, c.diff_up_cur), pst,
                          state.diff_left_base + off);

                if constexpr (!is_dual) {
                    auto smq = hn::Sub(c.score_max_cell, gap_open_vec);
                    c.diag_chain = hn::Sub(c.diag_chain, smq);
                    c.vert_chain = hn::Sub(c.vert_chain, smq);

                    auto dp = hn::Gt(c.diag_chain, zero_vec);
                    hn::Store(hn::IfThenElseZero(dp, c.diag_chain), pst,
                              state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(dp, bt_e_cont));

                    auto vp = hn::Gt(c.vert_chain, zero_vec);
                    hn::Store(hn::IfThenElseZero(vp, c.vert_chain), pst,
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
                              pst, state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(dp, bt_e_cont));

                    auto vp = hn::Gt(c.vert_chain, zero_vec);
                    hn::Store(hn::Sub(hn::IfThenElseZero(vp, c.vert_chain),
                                       short_gap_cost_vec),
                              pst, state.gap_f_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(vp, bt_f_cont));

                    auto d2p = hn::Gt(c.diag_chain2, zero_vec);
                    hn::Store(hn::Sub(hn::IfThenElseZero(d2p, c.diag_chain2),
                                       long_gap_cost_vec),
                              pst, state.gap_e2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(d2p, bt_e2_cont));

                    auto v2p = hn::Gt(c.vert_chain2, zero_vec);
                    hn::Store(hn::Sub(hn::IfThenElseZero(v2p, c.vert_chain2),
                                       long_gap_cost_vec),
                              pst, state.gap_f2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenElseZero(v2p, bt_f2_cont));
                }

                hn::Store(dir, pst,
                    backtrack.vec_slot_bytes(anti_diagonal, bvi - band_start_vec));
            }
        } else {
            // ---- CIGAR, gap right-alignment --------------------------------
            anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int bvi = band_start_vec; bvi <= band_end_vec; ++bvi) {
                const int off = bvi * bl;
                auto c = use_fused_score
                    ? ksw_dp_load<mode, true>(pst, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec, wildcard_rank_vec)
                    : ksw_dp_load<mode, false>(pst, off, state,
                        query_reversed_aligned, bx_vec, bv_vec, bx2_vec,
                        double_gap_cost_vec, match_score_vec,
                        mismatch_score_vec, wildcard_score_vec, wildcard_rank_vec);

                // Direction tracking — right-align tie-break
                auto dir = hn::IfThenZeroElse(
                    hn::Gt(c.score_max_cell, c.diag_chain), bt_dir_1);
                c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain);

                if constexpr (!is_dual) {
                    dir = hn::IfThenElse(
                        hn::Gt(c.score_max_cell, c.vert_chain), dir, bt_dir_2);
                    c.score_max_cell = max_u8(pst, c.score_max_cell, c.vert_chain);
                    c.score_max_cell = min_u8(pst, c.score_max_cell, max_plausible_score_vec);
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

                hn::Store(hn::Sub(c.score_max_cell, c.diff_left_sh), pst,
                          state.diff_up_base + off);
                hn::Store(hn::Sub(c.score_max_cell, c.diff_up_cur), pst,
                          state.diff_left_base + off);

                if constexpr (!is_dual) {
                    auto smq = hn::Sub(c.score_max_cell, gap_open_vec);
                    c.diag_chain = hn::Sub(c.diag_chain, smq);
                    c.vert_chain = hn::Sub(c.vert_chain, smq);

                    auto dn = hn::Gt(zero_vec, c.diag_chain);
                    hn::Store(hn::IfThenZeroElse(dn, c.diag_chain), pst,
                              state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(dn, bt_e_cont));

                    auto vn = hn::Gt(zero_vec, c.vert_chain);
                    hn::Store(hn::IfThenZeroElse(vn, c.vert_chain), pst,
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
                              pst, state.gap_e_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(dn, bt_e_cont));

                    auto vn = hn::Gt(zero_vec, c.vert_chain);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(vn, c.vert_chain),
                                       short_gap_cost_vec),
                              pst, state.gap_f_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(vn, bt_f_cont));

                    auto d2n = hn::Gt(zero_vec, c.diag_chain2);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(d2n, c.diag_chain2),
                                       long_gap_cost_vec),
                              pst, state.gap_e2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(d2n, bt_e2_cont));

                    auto v2n = hn::Gt(zero_vec, c.vert_chain2);
                    hn::Store(hn::Sub(hn::IfThenZeroElse(v2n, c.vert_chain2),
                                       long_gap_cost_vec),
                              pst, state.gap_f2_offset_base + off);
                    dir = hn::Or(dir, hn::IfThenZeroElse(v2n, bt_f2_cont));
                }

                hn::Store(dir, pst,
                    backtrack.vec_slot_bytes(anti_diagonal, bvi - band_start_vec));
            }
        }

        // ================================================================
        // Max score tracking and Z-drop
        // ================================================================
        const int8_t zdrop_gap = is_dual ? gap_extend_long : gap_extend;

        if (!use_approx_max) {
            full_score_t anti_diag_max_score;
            int anti_diag_max_target_pos;
            if (anti_diagonal > 0) {
                if constexpr (!is_dual) {
                    const auto* du = reinterpret_cast<const uint8_t*>(state.diff_up_base);
                    const auto* dl = reinterpret_cast<const uint8_t*>(state.diff_left_base);
                    anti_diag_max_score = exact_score_row[band_end_raw] = band_end_raw > 0
                        ? exact_score_row[band_end_raw - 1]
                          + static_cast<full_score_t>(du[band_end_raw]) - gap_open_plus_extend
                        : exact_score_row[band_end_raw]
                          + static_cast<full_score_t>(dl[band_end_raw]) - gap_open_plus_extend;
                } else {
                    anti_diag_max_score = exact_score_row[band_end_raw] = band_end_raw > 0
                        ? exact_score_row[band_end_raw - 1]
                          + static_cast<full_score_t>(state.diff_up_base[band_end_raw])
                        : exact_score_row[band_end_raw]
                          + static_cast<full_score_t>(state.diff_left_base[band_end_raw]);
                }
                anti_diag_max_target_pos = band_end_raw;

                auto max_sv = hn::Set(fst, anti_diag_max_score);
                auto max_pv = hn::Set(fst, static_cast<full_score_t>(anti_diag_max_target_pos));
                [[maybe_unused]] auto goe_vec = hn::Set(fst,
                    static_cast<full_score_t>(gap_open_plus_extend));

                const int block_end = band_start_raw
                    + (band_end_raw - band_start_raw) / static_cast<int>(score_lanes)
                      * static_cast<int>(score_lanes);
                int tp = band_start_raw;
                for (; tp < block_end; tp += static_cast<int>(score_lanes)) {
                    auto es = hn::LoadU(fst, &exact_score_row[tp]);
                    HWY_ALIGN full_score_t dl_buf[score_lanes_max] = {};
                    for (size_t li = 0; li < score_lanes; ++li) {
                        if constexpr (!is_dual)
                            dl_buf[li] = static_cast<full_score_t>(
                                reinterpret_cast<const uint8_t*>(state.diff_left_base)[tp + li]);
                        else
                            dl_buf[li] = static_cast<full_score_t>(
                                state.diff_left_base[tp + li]);
                    }
                    auto dlv = hn::Load(fst, dl_buf);
                    es = hn::Add(es, dlv);
                    if constexpr (!is_dual) es = hn::Sub(es, goe_vec);
                    hn::StoreU(es, fst, &exact_score_row[tp]);

                    auto tpv = hn::Set(fst, static_cast<full_score_t>(tp));
                    auto gm = hn::Gt(es, max_sv);
                    max_sv = hn::IfThenElse(gm, es, max_sv);
                    max_pv = hn::IfThenElse(gm, tpv, max_pv);
                }

                HWY_ALIGN full_score_t sb[score_lanes_max];
                HWY_ALIGN full_score_t pb[score_lanes_max];
                hn::Store(max_sv, fst, sb);
                hn::Store(max_pv, fst, pb);
                for (size_t li = 0; li < score_lanes; ++li) {
                    if (anti_diag_max_score < sb[li]) {
                        anti_diag_max_score = sb[li];
                        anti_diag_max_target_pos = static_cast<int>(pb[li])
                            + static_cast<int>(li);
                    }
                }
                for (; tp < band_end_raw; ++tp) {
                    if constexpr (!is_dual) {
                        exact_score_row[tp] += static_cast<full_score_t>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[tp])
                            - gap_open_plus_extend;
                    } else {
                        exact_score_row[tp] += static_cast<full_score_t>(
                            state.diff_left_base[tp]);
                    }
                    if (exact_score_row[tp] > anti_diag_max_score) {
                        anti_diag_max_score = exact_score_row[tp];
                        anti_diag_max_target_pos = tp;
                    }
                }
            } else {
                if constexpr (!is_dual) {
                    exact_score_row[0] = static_cast<full_score_t>(
                        reinterpret_cast<const uint8_t*>(state.diff_left_base)[0])
                        - gap_open_plus_extend - gap_open_plus_extend;
                } else {
                    exact_score_row[0] = static_cast<full_score_t>(
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
            if (apply_zdrop_heuristic(result, true,
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
                    full_score_t delta_left, delta_up;
                    if constexpr (!is_dual) {
                        delta_left = static_cast<full_score_t>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[approx_score_target_pos])
                            - gap_open_plus_extend;
                        delta_up = static_cast<full_score_t>(
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
                    if constexpr (!is_dual) {
                        approx_score += static_cast<full_score_t>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[approx_score_target_pos])
                            - gap_open_plus_extend;
                    } else {
                        approx_score += static_cast<full_score_t>(
                            state.diff_left_base[approx_score_target_pos]);
                    }
                } else {
                    ++approx_score_target_pos;
                    if constexpr (!is_dual) {
                        approx_score += static_cast<full_score_t>(
                            reinterpret_cast<const uint8_t*>(state.diff_up_base)[approx_score_target_pos])
                            - gap_open_plus_extend;
                    } else {
                        approx_score += static_cast<full_score_t>(
                            state.diff_up_base[approx_score_target_pos]);
                    }
                }
                if constexpr (!is_dual) {
                    if (flags.approx_zdrop
                        && apply_zdrop_heuristic(result, true,
                                                  approx_score, anti_diagonal,
                                                  approx_score_target_pos,
                                                  z_drop, zdrop_gap))
                    {
                        break;
                    }
                }
            } else {
                if constexpr (!is_dual) {
                    approx_score = static_cast<full_score_t>(
                        reinterpret_cast<const uint8_t*>(state.diff_left_base)[0])
                        - gap_open_plus_extend - gap_open_plus_extend;
                } else {
                    approx_score = static_cast<full_score_t>(
                        state.diff_left_base[0]) - gap_open_plus_extend;
                }
                approx_score_target_pos = 0;
            }
            if constexpr (is_dual) {
                if (flags.approx_zdrop
                    && apply_zdrop_heuristic(result, true,
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
        if (!result.zdropped && !flags.extz_only) {
            ksw_backtrack_cigar(backtrack, true, rev, 0,
                                tlen - 1, qlen - 1, result.cigar);
        } else if (!result.zdropped && flags.extz_only
                   && result.mqe + end_bonus > result.max)
        {
            result.reach_end = true;
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
// HwyKsw2Aligner — public API
// =============================================================================

class HwyKsw2Aligner {
public:
    template<
        bool is_reversed,
        supports_compact_sequence_range QueryRange,
        supports_compact_sequence_range TargetRange>
    [[nodiscard]] static KswAlignResult z2_align(
        const compact_sequence_range<QueryRange>& query,
        const compact_sequence_range<TargetRange>& target,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const ScoringMatrix<QueryRange>& scoring_matrix,
        const KswZ2Params& params)
    {
        static_assert(std::is_same_v<
            typename compact_sequence_range<QueryRange>::traits::alphabet_type,
            typename compact_sequence_range<TargetRange>::traits::alphabet_type>,
            "KSW2 requires query and target to share the same alphabet type.");
        return ksw2_core_align<KswAlignerMode::SINGLE_AFFINE, is_reversed>(
            query, target, query_start, query_end, target_start, target_end,
            scoring_matrix,
            params.gap_open, params.gap_extend,
            params.gap_open, params.gap_extend,
            params.band_width, params.z_drop, params.end_bonus, params.flags);
    }

    template<
        bool is_reversed,
        supports_compact_sequence_range QueryRange,
        supports_compact_sequence_range TargetRange>
    [[nodiscard]] static KswAlignResult d2_align(
        const compact_sequence_range<QueryRange>& query,
        const compact_sequence_range<TargetRange>& target,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const ScoringMatrix<QueryRange>& scoring_matrix,
        KswD2Params params)
    {
        static_assert(std::is_same_v<
            typename compact_sequence_range<QueryRange>::traits::alphabet_type,
            typename compact_sequence_range<TargetRange>::traits::alphabet_type>,
            "KSW2 requires query and target to share the same alphabet type.");
        if (params.gap_open_long + params.gap_extend_long
            < params.gap_open_short + params.gap_extend_short)
        {
            std::swap(params.gap_open_short, params.gap_open_long);
            std::swap(params.gap_extend_short, params.gap_extend_long);
        }
        return ksw2_core_align<KswAlignerMode::DUAL_AFFINE, is_reversed>(
            query, target, query_start, query_end, target_start, target_end,
            scoring_matrix,
            params.gap_open_short, params.gap_extend_short,
            params.gap_open_long, params.gap_extend_long,
            params.band_width, params.z_drop, params.end_bonus, params.flags);
    }
};

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
