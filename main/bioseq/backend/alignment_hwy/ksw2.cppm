/**
 * @file ksw2.cppm
 * @brief Highway (HWY) SIMD reimplementation of the KSW2 alignment algorithms.
 *
 * This module provides portable SIMD implementations of the KSW2 extension
 * alignment algorithms originally written with raw SSE2/SSE4.1 intrinsics.
 * By using Google Highway, the code compiles and runs on x86 (SSE2 through
 * AVX-512), ARM NEON/SVE, WASM, and any future target Highway supports.
 *
 * Exported entry point: HwyKsw2Aligner with static methods z2() and d2()
 * that are drop-in replacements for Aligner<ALIGN_ALGORITHM_KSW2>::z2/d2.
 *
 * The algorithms operate in anti-diagonal (rotated) coordinates with banded
 * dynamic programming. All SIMD vectors are fixed at 128 bits (16 int8 lanes)
 * to guarantee bit-exact results matching the original SSE implementation.
 *
 * Implemented algorithms:
 *   - z2: single-affine-gap extension alignment
 *   - d2: dual-affine-gap extension alignment
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

export module alignment_hwy;

import alignment;
import mlogging;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// Constants — vector size fixed to 128 bits for SSE-compatible results
// ============================================================================

/** Number of int8_t lanes per SIMD vector (always 16 for 128-bit). */
constexpr int KSW2_VEC_LANES = 16;

/** Number of bytes per SIMD vector. */
constexpr int KSW2_VEC_BYTES = 16;

// ============================================================================
// SIMD tag types — fixed 128-bit width for deterministic output
// ============================================================================

using D_I8  = hn::Full128<int8_t>;
using D_U8  = hn::Full128<uint8_t>;
using D_I16 = hn::Full128<int16_t>;
using D_U16 = hn::Full128<uint16_t>;
using D_I32 = hn::Full128<int32_t>;

using Vec_I8  = hn::Vec<D_I8>;
using Vec_U8  = hn::Vec<D_U8>;
using Vec_I32 = hn::Vec<D_I32>;

// ============================================================================
// Helper: unsigned max/min on int8_t vectors via BitCast
// ============================================================================

/**
 * @brief Unsigned 8-bit maximum, operating on int8_t-typed vectors.
 *
 * SSE equivalent: _mm_max_epu8(a, b)
 * Needed because KSW2's z2 algorithm uses a score bias that makes all
 * values non-negative, requiring unsigned comparison.
 */
inline Vec_I8 MaxU8(D_I8 d_i8, Vec_I8 a, Vec_I8 b) {
    D_U8 d_u8;
    return hn::BitCast(d_i8, hn::Max(hn::BitCast(d_u8, a), hn::BitCast(d_u8, b)));
}

/**
 * @brief Unsigned 8-bit minimum, operating on int8_t-typed vectors.
 *
 * SSE equivalent: _mm_min_epu8(a, b)
 */
inline Vec_I8 MinU8(D_I8 d_i8, Vec_I8 a, Vec_I8 b) {
    D_U8 d_u8;
    return hn::BitCast(d_i8, hn::Min(hn::BitCast(d_u8, a), hn::BitCast(d_u8, b)));
}

// ============================================================================
// Helper: create a vector with a single byte in lane 0, rest zero
// ============================================================================

/**
 * @brief Create a 128-bit vector with value in lane 0 and zeros elsewhere.
 *
 * SSE equivalent: _mm_cvtsi32_si128((uint8_t)val)
 * This pattern is used at the boundary of each anti-diagonal to propagate
 * the leftmost score from the previous iteration.
 */
inline Vec_I8 SetLane0(D_I8 d, int8_t val) {
    HWY_ALIGN int8_t buf[KSW2_VEC_LANES] = {};
    buf[0] = val;
    return hn::Load(d, buf);
}

// ============================================================================
// Helper: byte-shift-left-by-1 + insert lane 0 (stripe propagation)
// ============================================================================

/**
 * @brief Shift vector left by 1 byte and insert a new value at lane 0.
 *
 * This is the standard anti-diagonal propagation pattern:
 *   result = (vec << 1_byte) | [val, 0, 0, ..., 0]
 *
 * Also extracts the shifted-out byte (lane 15) for the next iteration.
 *
 * @param d        Tag for int8_t x16 vector.
 * @param vec      The vector to shift.
 * @param lane0    Reference to the lane-0 insert vector; updated to carry-out.
 * @return         The shifted vector with lane0's byte inserted at position 0.
 */
inline Vec_I8 ShiftLeftInsert(D_I8 d, Vec_I8 vec, Vec_I8& lane0) {
    auto carry_out = hn::ShiftRightBytes<15>(d, vec);
    auto shifted = hn::Or(hn::ShiftLeftBytes<1>(d, vec), lane0);
    lane0 = carry_out;
    return shifted;
}

// ============================================================================
// align_z2 — Single-affine-gap banded extension alignment (HWY port)
// ============================================================================

/**
 * @brief Single-affine-gap banded extension alignment using Highway SIMD.
 *
 * Port of ksw2_extz2_sse. Operates in anti-diagonal (rotated) coordinates
 * with 8-bit SIMD arithmetic and a score bias to keep values unsigned.
 *
 * Gap cost model: cost(k) = q + k * e for a gap of length k.
 *
 * The algorithm:
 *   1. Reverse query, copy target, allocate DP arrays.
 *   2. For each anti-diagonal r in [0, qlen+tlen-2]:
 *      a. Compute banded boundaries [st, en] in target space.
 *      b. Set boundary values for the leftmost/rightmost cells.
 *      c. Compute match/mismatch scores for the band.
 *      d. SIMD DP: propagate scores through u, v, x, y arrays.
 *      e. Track max score and apply Z-drop heuristic.
 *   3. Backtrack through the DP matrix to produce CIGAR (if requested).
 *
 * @param km         Memory pool handle (may be nullptr for standard malloc).
 *                   Forwarded to ksw_backtrack for CIGAR allocation.
 * @param qlen       Query length.
 * @param query      Query sequence (alphabet indices).
 * @param tlen       Target length.
 * @param target     Target sequence (alphabet indices).
 * @param m          Alphabet size.
 * @param mat        Scoring matrix (m x m, row-major).
 * @param q          Gap open penalty.
 * @param e          Gap extension penalty.
 * @param w          Band width (-1 for full).
 * @param zdrop      Z-drop threshold (-1 to disable).
 * @param end_bonus  Bonus for reaching query end.
 * @param flag       KSW_EZ_* flags.
 * @param ez         [out] Alignment result.
 */
void hwy_align_z2(
    void* km, int qlen, const uint8_t* query,
    int tlen, const uint8_t* target,
    int8_t m, const int8_t* mat,
    int8_t q, int8_t e, int w, int zdrop,
    int end_bonus, int flag, ksw_extz_t* ez)
{
    constexpr int VS = KSW2_VEC_BYTES;

    D_I8  d_i8;
    D_I32 d_i32;

    const int qe = q + e;
    const bool with_cigar = !(flag & KSW_EZ_SCORE_ONLY);
    const bool approx_max = !!(flag & KSW_EZ_APPROX_MAX);

    ksw_reset_extz(ez);
    if (m <= 0 || qlen <= 0 || tlen <= 0) return;

    // --- SIMD constant vectors ---
    const auto zero_vec   = hn::Zero(d_i8);
    const auto q_vec      = hn::Set(d_i8, q);
    const auto qe2_vec    = hn::Set(d_i8, static_cast<int8_t>((q + e) * 2));
    const auto flag1_vec  = hn::Set(d_i8, 1);
    const auto flag2_vec  = hn::Set(d_i8, 2);
    const auto flag8_vec  = hn::Set(d_i8, 0x08);
    const auto flag16_vec = hn::Set(d_i8, 0x10);
    const auto sc_mch_vec = hn::Set(d_i8, mat[0]);
    const auto sc_mis_vec = hn::Set(d_i8, mat[1]);
    const auto sc_N_vec   = mat[m * m - 1] == 0
                                ? hn::Set(d_i8, static_cast<int8_t>(-e))
                                : hn::Set(d_i8, mat[m * m - 1]);
    const auto m1_vec     = hn::Set(d_i8, static_cast<int8_t>(m - 1));
    const auto max_sc_vec = hn::Set(d_i8, static_cast<int8_t>(mat[0] + (q + e) * 2));

    // --- Band width ---
    if (w < 0) w = tlen > qlen ? tlen : qlen;
    int wl = w, wr = w;

    // --- Dimensions (16-element vector granularity) ---
    const int tlen_  = (tlen + 15) / 16;
    int n_col_ = qlen < tlen ? qlen : tlen;
    n_col_ = ((n_col_ < w + 1 ? n_col_ : w + 1) + 15) / 16 + 1;
    const int qlen_  = (qlen + 15) / 16;

    // --- Score range check ---
    int max_sc = mat[0], min_sc = mat[1];
    for (int t = 1; t < m * m; ++t) {
        max_sc = max_sc > mat[t] ? max_sc : mat[t];
        min_sc = min_sc < mat[t] ? min_sc : mat[t];
    }
    if (-min_sc > 2 * (q + e)) return;

    // --- Allocate SIMD working arrays (16-byte aligned) ---
    // Layout: u[tlen_] | v[tlen_] | x[tlen_] | y[tlen_] | s[tlen_] | sf[tlen_*16] | qr[qlen_*16]
    const size_t total_vecs = static_cast<size_t>(tlen_) * 6 + qlen_ + 1;
    auto mem = hwy::AllocateAligned<int8_t>(total_vecs * VS);
    std::memset(mem.get(), 0, total_vecs * VS);

    int8_t* u_ptr  = mem.get();
    int8_t* v_ptr  = u_ptr + tlen_ * VS;
    int8_t* x_ptr  = v_ptr + tlen_ * VS;
    int8_t* y_ptr  = x_ptr + tlen_ * VS;
    int8_t* s_ptr  = y_ptr + tlen_ * VS;
    uint8_t* sf    = reinterpret_cast<uint8_t*>(s_ptr + tlen_ * VS);
    uint8_t* qr    = sf + tlen_ * VS;

    // --- Exact max tracking (32-bit score array) ---
    std::vector<int32_t> H_vec;
    int32_t* H = nullptr;
    if (!approx_max) {
        H_vec.assign(tlen_ * 16, KSW_NEG_INF);
        H = H_vec.data();
    }

    // --- Backtrack matrix ---
    hwy::AlignedFreeUniquePtr<int8_t[]> p_mem;
    int8_t* p_ptr = nullptr;
    std::vector<int> off_vec;
    int* off = nullptr;
    int* off_end = nullptr;
    if (with_cigar) {
        const size_t p_size = (static_cast<size_t>(qlen + tlen - 1) * n_col_ + 1) * VS;
        p_mem = hwy::AllocateAligned<int8_t>(p_size);
        std::memset(p_mem.get(), 0, p_size);
        p_ptr = p_mem.get();
        off_vec.resize(static_cast<size_t>(qlen + tlen - 1) * 2);
        off = off_vec.data();
        off_end = off + qlen + tlen - 1;
    }

    // --- Prepare reversed query and target copy ---
    for (int t = 0; t < qlen; ++t) qr[t] = query[qlen - 1 - t];
    std::memcpy(sf, target, tlen);

    // --- Approximate max tracking ---
    int32_t H0 = 0, last_H0_t = 0;

    // ========================================================================
    // Main DP loop — one anti-diagonal per iteration
    // ========================================================================
    int last_st = -1, last_en = -1;
    for (int r = 0; r < qlen + tlen - 1; ++r) {
        int st = 0, en = tlen - 1;

        // --- Compute band boundaries ---
        if (st < r - qlen + 1) st = r - qlen + 1;
        if (en > r) en = r;
        if (st < (r - wr + 1) >> 1) st = (r - wr + 1) >> 1;
        if (en > (r + wl) >> 1) en = (r + wl) >> 1;
        if (st > en) {
            ez->zdropped = 1;
            break;
        }
        const int st0 = st, en0 = en;
        st = st / 16 * 16;
        en = (en + 16) / 16 * 16 - 1;

        // --- Boundary conditions ---
        int8_t x1_val, v1_val;
        uint8_t* qrr = qr + (qlen - 1 - r);
        uint8_t* u8 = reinterpret_cast<uint8_t*>(u_ptr);
        uint8_t* v8 = reinterpret_cast<uint8_t*>(v_ptr);

        if (st > 0) {
            if (st - 1 >= last_st && st - 1 <= last_en) {
                x1_val = reinterpret_cast<uint8_t*>(x_ptr)[st - 1];
                v1_val = v8[st - 1];
            } else {
                x1_val = 0;
                v1_val = 0;
            }
        } else {
            x1_val = 0;
            v1_val = r ? static_cast<int8_t>(q) : 0;
        }
        if (en >= r) {
            reinterpret_cast<uint8_t*>(y_ptr)[r] = 0;
            u8[r] = r ? static_cast<uint8_t>(q) : 0;
        }

        // --- Compute match/mismatch scores for this anti-diagonal ---
        if (!(flag & KSW_EZ_GENERIC_SC)) {
            for (int t = st0; t <= en0; t += 16) {
                auto sq = hn::LoadU(d_i8, reinterpret_cast<const int8_t*>(&sf[t]));
                auto st_v = hn::LoadU(d_i8, reinterpret_cast<const int8_t*>(&qrr[t]));
                auto mask_wild = hn::Or(
                    hn::VecFromMask(d_i8, hn::Eq(sq, m1_vec)),
                    hn::VecFromMask(d_i8, hn::Eq(st_v, m1_vec)));
                auto eq_mask = hn::VecFromMask(d_i8, hn::Eq(sq, st_v));
                // score = eq ? match : mismatch; if wildcard, use N score
                auto score = hn::IfThenElse(
                    hn::MaskFromVec(eq_mask), sc_mch_vec, sc_mis_vec);
                score = hn::IfThenElse(
                    hn::MaskFromVec(mask_wild), sc_N_vec, score);
                hn::StoreU(score, d_i8, s_ptr + t);
            }
        } else {
            for (int t = st0; t <= en0; ++t) {
                reinterpret_cast<uint8_t*>(s_ptr)[t] = mat[sf[t] * m + qrr[t]];
            }
        }

        // --- Initialize boundary vectors for stripe propagation ---
        auto x1_vec = SetLane0(d_i8, x1_val);
        auto v1_vec = SetLane0(d_i8, v1_val);
        const int st_ = st / 16, en_ = en / 16;
        DEBUG_ASSERT(en_ - st_ + 1 <= n_col_, "Band width exceeds allocated column count");

        // ================================================================
        // Core DP inner loop over SIMD vectors in the band
        // ================================================================
        if (!with_cigar) {
            // --- Score-only path (no backtrack) ---
            for (int t = st_; t <= en_; ++t) {
                // dp_code_block1: load scores, propagate diagonals
                auto z = hn::Add(hn::Load(d_i8, s_ptr + t * VS), qe2_vec);
                auto xt1 = hn::Load(d_i8, x_ptr + t * VS);
                xt1 = ShiftLeftInsert(d_i8, xt1, x1_vec);
                auto vt1 = hn::Load(d_i8, v_ptr + t * VS);
                vt1 = ShiftLeftInsert(d_i8, vt1, v1_vec);
                auto a = hn::Add(xt1, vt1);
                auto ut = hn::Load(d_i8, u_ptr + t * VS);
                auto b = hn::Add(hn::Load(d_i8, y_ptr + t * VS), ut);

                // z = max(z, a) using signed max (HWY provides this natively)
                z = hn::Max(z, a);
                // dp_code_block2: unsigned max(z, b), clamp, store u/v, update a/b
                z = MaxU8(d_i8, z, b);
                z = MinU8(d_i8, z, max_sc_vec);
                hn::Store(hn::Sub(z, vt1), d_i8, u_ptr + t * VS);
                hn::Store(hn::Sub(z, ut), d_i8, v_ptr + t * VS);
                z = hn::Sub(z, q_vec);
                a = hn::Sub(a, z);
                b = hn::Sub(b, z);

                hn::Store(hn::Max(a, zero_vec), d_i8, x_ptr + t * VS);
                hn::Store(hn::Max(b, zero_vec), d_i8, y_ptr + t * VS);
            }
        } else if (!(flag & KSW_EZ_RIGHT)) {
            // --- CIGAR path, gap left-alignment ---
            int8_t* pr = p_ptr + static_cast<size_t>(r) * n_col_ * VS - st_ * VS;
            off[r] = st;
            off_end[r] = en;
            for (int t = st_; t <= en_; ++t) {
                // dp_code_block1
                auto z = hn::Add(hn::Load(d_i8, s_ptr + t * VS), qe2_vec);
                auto xt1 = hn::Load(d_i8, x_ptr + t * VS);
                xt1 = ShiftLeftInsert(d_i8, xt1, x1_vec);
                auto vt1 = hn::Load(d_i8, v_ptr + t * VS);
                vt1 = ShiftLeftInsert(d_i8, vt1, v1_vec);
                auto a = hn::Add(xt1, vt1);
                auto ut = hn::Load(d_i8, u_ptr + t * VS);
                auto b = hn::Add(hn::Load(d_i8, y_ptr + t * VS), ut);

                // d = a > z ? 1 : 0
                auto d_vec = hn::IfThenElseZero(hn::Gt(a, z), flag1_vec);
                // z = max(z, a) (signed)
                z = hn::Max(z, a);
                // d = b > z ? 2 : d
                auto gt_bz = hn::Gt(b, z);
                d_vec = hn::IfThenElse(gt_bz, flag2_vec, d_vec);

                // dp_code_block2
                z = MaxU8(d_i8, z, b);
                z = MinU8(d_i8, z, max_sc_vec);
                hn::Store(hn::Sub(z, vt1), d_i8, u_ptr + t * VS);
                hn::Store(hn::Sub(z, ut), d_i8, v_ptr + t * VS);
                z = hn::Sub(z, q_vec);
                a = hn::Sub(a, z);
                b = hn::Sub(b, z);

                // Store x with continuation flag
                auto a_gt_zero = hn::Gt(a, zero_vec);
                hn::Store(hn::IfThenElseZero(a_gt_zero, a), d_i8, x_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenElseZero(a_gt_zero, flag8_vec));

                // Store y with continuation flag
                auto b_gt_zero = hn::Gt(b, zero_vec);
                hn::Store(hn::IfThenElseZero(b_gt_zero, b), d_i8, y_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenElseZero(b_gt_zero, flag16_vec));

                hn::Store(d_vec, d_i8, pr + t * VS);
            }
        } else {
            // --- CIGAR path, gap right-alignment ---
            int8_t* pr = p_ptr + static_cast<size_t>(r) * n_col_ * VS - st_ * VS;
            off[r] = st;
            off_end[r] = en;
            for (int t = st_; t <= en_; ++t) {
                // dp_code_block1
                auto z = hn::Add(hn::Load(d_i8, s_ptr + t * VS), qe2_vec);
                auto xt1 = hn::Load(d_i8, x_ptr + t * VS);
                xt1 = ShiftLeftInsert(d_i8, xt1, x1_vec);
                auto vt1 = hn::Load(d_i8, v_ptr + t * VS);
                vt1 = ShiftLeftInsert(d_i8, vt1, v1_vec);
                auto a = hn::Add(xt1, vt1);
                auto ut = hn::Load(d_i8, u_ptr + t * VS);
                auto b = hn::Add(hn::Load(d_i8, y_ptr + t * VS), ut);

                // d = z > a ? 0 : 1  (right-align: prefer gap on the right)
                auto d_vec = hn::IfThenZeroElse(hn::Gt(z, a), flag1_vec);
                // z = max(z, a) (signed)
                z = hn::Max(z, a);
                // d = z > b ? d : 2
                auto gt_zb = hn::Gt(z, b);
                d_vec = hn::IfThenElse(gt_zb, d_vec, flag2_vec);

                // dp_code_block2
                z = MaxU8(d_i8, z, b);
                z = MinU8(d_i8, z, max_sc_vec);
                hn::Store(hn::Sub(z, vt1), d_i8, u_ptr + t * VS);
                hn::Store(hn::Sub(z, ut), d_i8, v_ptr + t * VS);
                z = hn::Sub(z, q_vec);
                a = hn::Sub(a, z);
                b = hn::Sub(b, z);

                // Right-alignment: d |= (0 > a) ? 0 : 0x08
                auto zero_gt_a = hn::Gt(zero_vec, a);
                hn::Store(hn::IfThenZeroElse(zero_gt_a, a), d_i8, x_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenZeroElse(zero_gt_a, flag8_vec));

                auto zero_gt_b = hn::Gt(zero_vec, b);
                hn::Store(hn::IfThenZeroElse(zero_gt_b, b), d_i8, y_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenZeroElse(zero_gt_b, flag16_vec));

                hn::Store(d_vec, d_i8, pr + t * VS);
            }
        }

        // ================================================================
        // Max score tracking and Z-drop
        // ================================================================
        if (!approx_max) {
            int32_t max_H, max_t;
            if (r > 0) {
                max_H = H[en0] = en0 > 0
                    ? H[en0 - 1] + static_cast<int32_t>(u8[en0]) - qe
                    : H[en0] + static_cast<int32_t>(v8[en0]) - qe;
                max_t = en0;

                auto max_H_v = hn::Set(d_i32, max_H);
                auto max_t_v = hn::Set(d_i32, max_t);
                const auto qe_v = hn::Set(d_i32, q + e);

                const int en1 = st0 + (en0 - st0) / 4 * 4;
                int t = st0;
                for (; t < en1; t += 4) {
                    auto H1 = hn::LoadU(d_i32, &H[t]);
                    HWY_ALIGN int32_t v_arr[4] = {
                        static_cast<int32_t>(v8[t]),
                        static_cast<int32_t>(v8[t + 1]),
                        static_cast<int32_t>(v8[t + 2]),
                        static_cast<int32_t>(v8[t + 3])
                    };
                    auto v_inc = hn::Load(d_i32, v_arr);
                    H1 = hn::Add(H1, v_inc);
                    H1 = hn::Sub(H1, qe_v);
                    hn::StoreU(H1, d_i32, &H[t]);

                    auto t_bcast = hn::Set(d_i32, t);
                    auto cmp = hn::Gt(H1, max_H_v);
                    max_H_v = hn::IfThenElse(cmp, H1, max_H_v);
                    max_t_v = hn::IfThenElse(cmp, t_bcast, max_t_v);
                }

                HWY_ALIGN int32_t HH[4], tt[4];
                hn::Store(max_H_v, d_i32, HH);
                hn::Store(max_t_v, d_i32, tt);
                for (int i = 0; i < 4; ++i) {
                    if (max_H < HH[i]) {
                        max_H = HH[i];
                        max_t = tt[i] + i;
                    }
                }
                for (; t < en0; ++t) {
                    H[t] += static_cast<int32_t>(v8[t]) - qe;
                    if (H[t] > max_H) {
                        max_H = H[t];
                        max_t = t;
                    }
                }
            } else {
                H[0] = static_cast<int32_t>(v8[0]) - qe - qe;
                max_H = H[0];
                max_t = 0;
            }
            if (en0 == tlen - 1 && H[en0] > ez->mte) {
                ez->mte = H[en0];
                ez->mte_q = r - en0;
            }
            if (r - st0 == qlen - 1 && H[st0] > ez->mqe) {
                ez->mqe = H[st0];
                ez->mqe_t = st0;
            }
            if (ksw_apply_zdrop(ez, 1, max_H, r, max_t, zdrop, e)) break;
            if (r == qlen + tlen - 2 && en0 == tlen - 1) {
                ez->score = H[tlen - 1];
            }
        } else {
            // Approximate max
            if (r > 0) {
                if (last_H0_t >= st0 && last_H0_t <= en0
                    && last_H0_t + 1 >= st0 && last_H0_t + 1 <= en0) {
                    int32_t d0 = v8[last_H0_t] - qe;
                    int32_t d1 = u8[last_H0_t + 1] - qe;
                    if (d0 > d1) H0 += d0;
                    else { H0 += d1; ++last_H0_t; }
                } else if (last_H0_t >= st0 && last_H0_t <= en0) {
                    H0 += static_cast<int32_t>(v8[last_H0_t]) - qe;
                } else {
                    ++last_H0_t;
                    H0 += static_cast<int32_t>(u8[last_H0_t]) - qe;
                }
                if ((flag & KSW_EZ_APPROX_DROP)
                    && ksw_apply_zdrop(ez, 1, H0, r, last_H0_t, zdrop, e))
                    break;
            } else {
                H0 = static_cast<int32_t>(v8[0]) - qe - qe;
                last_H0_t = 0;
            }
            if (r == qlen + tlen - 2 && en0 == tlen - 1) {
                ez->score = H0;
            }
        }
        last_st = st;
        last_en = en;
    }

    // ========================================================================
    // Backtrack to produce CIGAR
    // ========================================================================
    if (with_cigar) {
        const int rev_cigar = !!(flag & KSW_EZ_REV_CIGAR);
        if (!ez->zdropped && !(flag & KSW_EZ_EXTZ_ONLY)) {
            ksw_backtrack(km, 1, rev_cigar, 0,
                reinterpret_cast<uint8_t*>(p_ptr), off, off_end,
                n_col_ * 16, tlen - 1, qlen - 1,
                &ez->m_cigar, &ez->n_cigar, &ez->cigar);
        } else if (!ez->zdropped && (flag & KSW_EZ_EXTZ_ONLY)
                   && ez->mqe + end_bonus > static_cast<int>(ez->max)) {
            ez->reach_end = 1;
            ksw_backtrack(km, 1, rev_cigar, 0,
                reinterpret_cast<uint8_t*>(p_ptr), off, off_end,
                n_col_ * 16, ez->mqe_t, qlen - 1,
                &ez->m_cigar, &ez->n_cigar, &ez->cigar);
        } else if (ez->max_t >= 0 && ez->max_q >= 0) {
            ksw_backtrack(km, 1, rev_cigar, 0,
                reinterpret_cast<uint8_t*>(p_ptr), off, off_end,
                n_col_ * 16, ez->max_t, ez->max_q,
                &ez->m_cigar, &ez->n_cigar, &ez->cigar);
        }
    }
}

// ============================================================================
// align_d2 — Dual-affine-gap banded extension alignment (HWY port)
// ============================================================================

/**
 * @brief Dual-affine-gap banded extension alignment using Highway SIMD.
 *
 * Port of ksw2_extd2_sse. Uses two affine gap penalty functions:
 *   gap_cost = min(q + k*e, q2 + k*e2)
 * This efficiently models both short gaps (q+k*e) and long gaps (q2+k*e2).
 *
 * Additional DP arrays x2, y2 track the second gap penalty state.
 * All SIMD arithmetic is signed 8-bit (no unsigned bias, unlike z2).
 *
 * The algorithm structure is identical to z2 with the following changes:
 *   - Two pairs of gap penalties (q/e and q2/e2), swapped if needed
 *   - Additional x2/y2 arrays for the second gap state
 *   - Different score update formula in the DP code blocks
 *   - Backtrack encodes 5 states (0=H, 1=E, 2=F, 3=E2, 4=F2)
 *
 * @param km         Memory pool handle (may be nullptr for standard malloc).
 *                   Forwarded to ksw_backtrack for CIGAR allocation.
 * @param qlen       Query length.
 * @param query      Query sequence (alphabet indices).
 * @param tlen       Target length.
 * @param target     Target sequence (alphabet indices).
 * @param m          Alphabet size.
 * @param mat        Scoring matrix (m x m, row-major).
 * @param q          First gap open penalty.
 * @param e          First gap extension penalty.
 * @param q2         Second gap open penalty (long gaps).
 * @param e2         Second gap extension penalty (long gaps).
 * @param w          Band width (-1 for full).
 * @param zdrop      Z-drop threshold (-1 to disable).
 * @param end_bonus  Bonus for reaching query end.
 * @param flag       KSW_EZ_* flags.
 * @param ez         [out] Alignment result.
 */
void hwy_align_d2(
    void* km, int qlen, const uint8_t* query,
    int tlen, const uint8_t* target,
    int8_t m, const int8_t* mat,
    int8_t q, int8_t e, int8_t q2, int8_t e2,
    int w, int zdrop, int end_bonus, int flag,
    ksw_extz_t* ez)
{
    constexpr int VS = KSW2_VEC_BYTES;

    D_I8  d_i8;
    D_I32 d_i32;

    const bool with_cigar = !(flag & KSW_EZ_SCORE_ONLY);
    const bool approx_max = !!(flag & KSW_EZ_APPROX_MAX);

    ksw_reset_extz(ez);
    if (m <= 1 || qlen <= 0 || tlen <= 0) return;

    // Ensure q+e <= q2+e2 (swap if needed)
    if (q2 + e2 < q + e) {
        int8_t tmp;
        tmp = q; q = q2; q2 = tmp;
        tmp = e; e = e2; e2 = tmp;
    }
    const int qe = q + e;

    // --- SIMD constant vectors ---
    const auto zero_vec   = hn::Zero(d_i8);
    const auto q_vec      = hn::Set(d_i8, q);
    const auto q2_vec     = hn::Set(d_i8, q2);
    const auto qe_vec     = hn::Set(d_i8, static_cast<int8_t>(q + e));
    const auto qe2_vec    = hn::Set(d_i8, static_cast<int8_t>(q2 + e2));
    const auto sc_mch_vec = hn::Set(d_i8, mat[0]);
    const auto sc_mis_vec = hn::Set(d_i8, mat[1]);
    const auto sc_N_vec   = mat[m * m - 1] == 0
                                ? hn::Set(d_i8, static_cast<int8_t>(-e2))
                                : hn::Set(d_i8, mat[m * m - 1]);
    const auto m1_vec     = hn::Set(d_i8, static_cast<int8_t>(m - 1));

    // Flag constant vectors for backtrack encoding
    const auto flag1_vec  = hn::Set(d_i8, 1);
    const auto flag2_vec  = hn::Set(d_i8, 2);
    const auto flag3_vec  = hn::Set(d_i8, 3);
    const auto flag4_vec  = hn::Set(d_i8, 4);
    const auto flag08_vec = hn::Set(d_i8, 0x08);
    const auto flag10_vec = hn::Set(d_i8, 0x10);
    const auto flag20_vec = hn::Set(d_i8, 0x20);
    const auto flag40_vec = hn::Set(d_i8, 0x40);

    // --- Band width ---
    if (w < 0) w = tlen > qlen ? tlen : qlen;
    int wl = w, wr = w;

    // --- Dimensions ---
    const int tlen_  = (tlen + 15) / 16;
    int n_col_ = qlen < tlen ? qlen : tlen;
    n_col_ = ((n_col_ < w + 1 ? n_col_ : w + 1) + 15) / 16 + 1;
    const int qlen_  = (qlen + 15) / 16;

    // --- Score range check ---
    int max_sc = mat[0], min_sc = mat[1];
    for (int t = 1; t < m * m; ++t) {
        max_sc = max_sc > mat[t] ? max_sc : mat[t];
        min_sc = min_sc < mat[t] ? min_sc : mat[t];
    }
    if (-min_sc > 2 * (q + e)) return;

    // --- Long-gap threshold ---
    const int long_thres = e != e2
        ? [&]() {
            int lt = (q2 - q) / (e - e2) - 1;
            if (q2 + e2 + lt * e2 > q + e + lt * e) ++lt;
            return lt;
          }()
        : 0;
    const int long_diff = long_thres * (e - e2) - (q2 - q) - e2;

    // --- Allocate SIMD working arrays ---
    // Layout: u | v | x | y | x2 | y2 | s | sf | qr
    const size_t total_vecs = static_cast<size_t>(tlen_) * 8 + qlen_ + 1;
    auto mem = hwy::AllocateAligned<int8_t>(total_vecs * VS);

    int8_t* u_ptr   = mem.get();
    int8_t* v_ptr   = u_ptr  + tlen_ * VS;
    int8_t* x_ptr   = v_ptr  + tlen_ * VS;
    int8_t* y_ptr   = x_ptr  + tlen_ * VS;
    int8_t* x2_ptr  = y_ptr  + tlen_ * VS;
    int8_t* y2_ptr  = x2_ptr + tlen_ * VS;
    int8_t* s_ptr   = y2_ptr + tlen_ * VS;
    uint8_t* sf     = reinterpret_cast<uint8_t*>(s_ptr + tlen_ * VS);
    uint8_t* qr     = sf + tlen_ * VS;

    // Initialize DP arrays to gap-penalty values (signed, stored as bytes).
    // Note: s_ptr, sf, qr are NOT zero-initialized — they are fully written
    // before being read (score loop writes s_ptr, memcpy writes sf, loop writes qr).
    std::memset(u_ptr,  static_cast<uint8_t>(-q - e),   tlen_ * VS);
    std::memset(v_ptr,  static_cast<uint8_t>(-q - e),   tlen_ * VS);
    std::memset(x_ptr,  static_cast<uint8_t>(-q - e),   tlen_ * VS);
    std::memset(y_ptr,  static_cast<uint8_t>(-q - e),   tlen_ * VS);
    std::memset(x2_ptr, static_cast<uint8_t>(-q2 - e2), tlen_ * VS);
    std::memset(y2_ptr, static_cast<uint8_t>(-q2 - e2), tlen_ * VS);

    // --- Exact max tracking ---
    std::vector<int32_t> H_vec;
    int32_t* H = nullptr;
    if (!approx_max) {
        H_vec.assign(tlen_ * 16, KSW_NEG_INF);
        H = H_vec.data();
    }

    // --- Backtrack matrix ---
    hwy::AlignedFreeUniquePtr<int8_t[]> p_mem;
    int8_t* p_ptr = nullptr;
    std::vector<int> off_vec;
    int* off = nullptr;
    int* off_end = nullptr;
    if (with_cigar) {
        const size_t p_size = (static_cast<size_t>(qlen + tlen - 1) * n_col_ + 1) * VS;
        p_mem = hwy::AllocateAligned<int8_t>(p_size);
        std::memset(p_mem.get(), 0, p_size);
        p_ptr = p_mem.get();
        off_vec.resize(static_cast<size_t>(qlen + tlen - 1) * 2);
        off = off_vec.data();
        off_end = off + qlen + tlen - 1;
    }

    // --- Prepare sequences ---
    for (int t = 0; t < qlen; ++t) qr[t] = query[qlen - 1 - t];
    std::memcpy(sf, target, tlen);

    // --- Approximate max tracking ---
    int32_t H0 = 0, last_H0_t = 0;

    // ========================================================================
    // Main DP loop
    // ========================================================================
    int last_st = -1, last_en = -1;
    for (int r = 0; r < qlen + tlen - 1; ++r) {
        int st = 0, en = tlen - 1;

        // --- Band boundaries ---
        if (st < r - qlen + 1) st = r - qlen + 1;
        if (en > r) en = r;
        if (st < (r - wr + 1) >> 1) st = (r - wr + 1) >> 1;
        if (en > (r + wl) >> 1) en = (r + wl) >> 1;
        if (st > en) {
            ez->zdropped = 1;
            break;
        }
        const int st0 = st, en0 = en;
        st = st / 16 * 16;
        en = (en + 16) / 16 * 16 - 1;

        // --- Boundary conditions (signed 8-bit) ---
        int8_t x1_val, x21_val, v1_val;
        uint8_t* qrr = qr + (qlen - 1 - r);
        int8_t* u8 = reinterpret_cast<int8_t*>(u_ptr);
        int8_t* v8 = reinterpret_cast<int8_t*>(v_ptr);
        int8_t* x8 = reinterpret_cast<int8_t*>(x_ptr);
        int8_t* x28 = reinterpret_cast<int8_t*>(x2_ptr);

        if (st > 0) {
            if (st - 1 >= last_st && st - 1 <= last_en) {
                x1_val  = x8[st - 1];
                x21_val = x28[st - 1];
                v1_val  = v8[st - 1];
            } else {
                x1_val  = static_cast<int8_t>(-q - e);
                x21_val = static_cast<int8_t>(-q2 - e2);
                v1_val  = static_cast<int8_t>(-q - e);
            }
        } else {
            x1_val  = static_cast<int8_t>(-q - e);
            x21_val = static_cast<int8_t>(-q2 - e2);
            v1_val  = r == 0 ? static_cast<int8_t>(-q - e)
                    : r < long_thres ? static_cast<int8_t>(-e)
                    : r == long_thres ? static_cast<int8_t>(long_diff)
                    : static_cast<int8_t>(-e2);
        }
        if (en >= r) {
            reinterpret_cast<int8_t*>(y_ptr)[r] = static_cast<int8_t>(-q - e);
            reinterpret_cast<int8_t*>(y2_ptr)[r] = static_cast<int8_t>(-q2 - e2);
            u8[r] = r == 0 ? static_cast<int8_t>(-q - e)
                  : r < long_thres ? static_cast<int8_t>(-e)
                  : r == long_thres ? static_cast<int8_t>(long_diff)
                  : static_cast<int8_t>(-e2);
        }

        // --- Compute scores ---
        if (!(flag & KSW_EZ_GENERIC_SC)) {
            for (int t = st0; t <= en0; t += 16) {
                auto sq = hn::LoadU(d_i8, reinterpret_cast<const int8_t*>(&sf[t]));
                auto st_v = hn::LoadU(d_i8, reinterpret_cast<const int8_t*>(&qrr[t]));
                auto mask_wild = hn::Or(
                    hn::VecFromMask(d_i8, hn::Eq(sq, m1_vec)),
                    hn::VecFromMask(d_i8, hn::Eq(st_v, m1_vec)));
                auto eq_mask = hn::VecFromMask(d_i8, hn::Eq(sq, st_v));
                auto score = hn::IfThenElse(
                    hn::MaskFromVec(eq_mask), sc_mch_vec, sc_mis_vec);
                score = hn::IfThenElse(
                    hn::MaskFromVec(mask_wild), sc_N_vec, score);
                hn::StoreU(score, d_i8, s_ptr + t);
            }
        } else {
            for (int t = st0; t <= en0; ++t) {
                reinterpret_cast<uint8_t*>(s_ptr)[t] = mat[sf[t] * m + qrr[t]];
            }
        }

        // --- Boundary vectors ---
        auto x1_vec  = SetLane0(d_i8, x1_val);
        auto x21_vec = SetLane0(d_i8, x21_val);
        auto v1_vec  = SetLane0(d_i8, v1_val);
        const int st_ = st / 16, en_ = en / 16;
        DEBUG_ASSERT(en_ - st_ + 1 <= n_col_, "Band width exceeds allocated column count");

        // ================================================================
        // Core DP
        // ================================================================
        if (!with_cigar) {
            // --- Score-only ---
            for (int t = st_; t <= en_; ++t) {
                // dp_code_block1 (d2 version: no qe2 addition, adds x2/y2)
                auto z = hn::Load(d_i8, s_ptr + t * VS);
                auto xt1 = hn::Load(d_i8, x_ptr + t * VS);
                xt1 = ShiftLeftInsert(d_i8, xt1, x1_vec);
                auto vt1 = hn::Load(d_i8, v_ptr + t * VS);
                vt1 = ShiftLeftInsert(d_i8, vt1, v1_vec);
                auto a = hn::Add(xt1, vt1);
                auto ut = hn::Load(d_i8, u_ptr + t * VS);
                auto b = hn::Add(hn::Load(d_i8, y_ptr + t * VS), ut);

                auto x2t1 = hn::Load(d_i8, x2_ptr + t * VS);
                x2t1 = ShiftLeftInsert(d_i8, x2t1, x21_vec);
                auto a2 = hn::Add(x2t1, vt1);
                auto b2 = hn::Add(hn::Load(d_i8, y2_ptr + t * VS), ut);

                // z = max(z, a, b, a2, b2) — all signed
                z = hn::Max(z, a);
                z = hn::Max(z, b);
                z = hn::Max(z, a2);
                z = hn::Max(z, b2);
                z = hn::Min(z, sc_mch_vec);

                // dp_code_block2 (d2 version)
                hn::Store(hn::Sub(z, vt1), d_i8, u_ptr + t * VS);
                hn::Store(hn::Sub(z, ut), d_i8, v_ptr + t * VS);
                auto tmp_q  = hn::Sub(z, q_vec);
                a  = hn::Sub(a,  tmp_q);
                b  = hn::Sub(b,  tmp_q);
                auto tmp_q2 = hn::Sub(z, q2_vec);
                a2 = hn::Sub(a2, tmp_q2);
                b2 = hn::Sub(b2, tmp_q2);

                hn::Store(hn::Sub(hn::Max(a,  zero_vec), qe_vec),  d_i8, x_ptr  + t * VS);
                hn::Store(hn::Sub(hn::Max(b,  zero_vec), qe_vec),  d_i8, y_ptr  + t * VS);
                hn::Store(hn::Sub(hn::Max(a2, zero_vec), qe2_vec), d_i8, x2_ptr + t * VS);
                hn::Store(hn::Sub(hn::Max(b2, zero_vec), qe2_vec), d_i8, y2_ptr + t * VS);
            }
        } else if (!(flag & KSW_EZ_RIGHT)) {
            // --- CIGAR, gap left-alignment ---
            int8_t* pr = p_ptr + static_cast<size_t>(r) * n_col_ * VS - st_ * VS;
            off[r] = st;
            off_end[r] = en;
            for (int t = st_; t <= en_; ++t) {
                auto z = hn::Load(d_i8, s_ptr + t * VS);
                auto xt1 = hn::Load(d_i8, x_ptr + t * VS);
                xt1 = ShiftLeftInsert(d_i8, xt1, x1_vec);
                auto vt1 = hn::Load(d_i8, v_ptr + t * VS);
                vt1 = ShiftLeftInsert(d_i8, vt1, v1_vec);
                auto a = hn::Add(xt1, vt1);
                auto ut = hn::Load(d_i8, u_ptr + t * VS);
                auto b = hn::Add(hn::Load(d_i8, y_ptr + t * VS), ut);
                auto x2t1 = hn::Load(d_i8, x2_ptr + t * VS);
                x2t1 = ShiftLeftInsert(d_i8, x2t1, x21_vec);
                auto a2 = hn::Add(x2t1, vt1);
                auto b2 = hn::Add(hn::Load(d_i8, y2_ptr + t * VS), ut);

                // Backtrack direction: find which state achieves max (left-align)
                auto d_vec = hn::IfThenElseZero(hn::Gt(a, z), flag1_vec);   // a>z  → 1
                z = hn::Max(z, a);
                d_vec = hn::IfThenElse(hn::Gt(b, z), flag2_vec, d_vec);     // b>z  → 2
                z = hn::Max(z, b);
                d_vec = hn::IfThenElse(hn::Gt(a2, z), flag3_vec, d_vec);    // a2>z → 3
                z = hn::Max(z, a2);
                d_vec = hn::IfThenElse(hn::Gt(b2, z), flag4_vec, d_vec);    // b2>z → 4
                z = hn::Max(z, b2);
                z = hn::Min(z, sc_mch_vec);

                // dp_code_block2 (d2)
                hn::Store(hn::Sub(z, vt1), d_i8, u_ptr + t * VS);
                hn::Store(hn::Sub(z, ut), d_i8, v_ptr + t * VS);
                auto tmp_q  = hn::Sub(z, q_vec);
                a  = hn::Sub(a,  tmp_q);
                b  = hn::Sub(b,  tmp_q);
                auto tmp_q2 = hn::Sub(z, q2_vec);
                a2 = hn::Sub(a2, tmp_q2);
                b2 = hn::Sub(b2, tmp_q2);

                // Store gap states with continuation flags
                auto a_pos = hn::Gt(a, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(a_pos, a), qe_vec), d_i8, x_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenElseZero(a_pos, flag08_vec));

                auto b_pos = hn::Gt(b, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(b_pos, b), qe_vec), d_i8, y_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenElseZero(b_pos, flag10_vec));

                auto a2_pos = hn::Gt(a2, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(a2_pos, a2), qe2_vec), d_i8, x2_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenElseZero(a2_pos, flag20_vec));

                auto b2_pos = hn::Gt(b2, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(b2_pos, b2), qe2_vec), d_i8, y2_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenElseZero(b2_pos, flag40_vec));

                hn::Store(d_vec, d_i8, pr + t * VS);
            }
        } else {
            // --- CIGAR, gap right-alignment ---
            int8_t* pr = p_ptr + static_cast<size_t>(r) * n_col_ * VS - st_ * VS;
            off[r] = st;
            off_end[r] = en;
            for (int t = st_; t <= en_; ++t) {
                auto z = hn::Load(d_i8, s_ptr + t * VS);
                auto xt1 = hn::Load(d_i8, x_ptr + t * VS);
                xt1 = ShiftLeftInsert(d_i8, xt1, x1_vec);
                auto vt1 = hn::Load(d_i8, v_ptr + t * VS);
                vt1 = ShiftLeftInsert(d_i8, vt1, v1_vec);
                auto a = hn::Add(xt1, vt1);
                auto ut = hn::Load(d_i8, u_ptr + t * VS);
                auto b = hn::Add(hn::Load(d_i8, y_ptr + t * VS), ut);
                auto x2t1 = hn::Load(d_i8, x2_ptr + t * VS);
                x2t1 = ShiftLeftInsert(d_i8, x2t1, x21_vec);
                auto a2 = hn::Add(x2t1, vt1);
                auto b2 = hn::Add(hn::Load(d_i8, y2_ptr + t * VS), ut);

                // Right-align: prefer gaps on the right (opposite tie-break)
                auto d_vec = hn::IfThenZeroElse(hn::Gt(z, a), flag1_vec);   // z>a  → 0, else 1
                z = hn::Max(z, a);
                d_vec = hn::IfThenElse(hn::Gt(z, b), d_vec, flag2_vec);     // z>b  → d, else 2
                z = hn::Max(z, b);
                d_vec = hn::IfThenElse(hn::Gt(z, a2), d_vec, flag3_vec);    // z>a2 → d, else 3
                z = hn::Max(z, a2);
                d_vec = hn::IfThenElse(hn::Gt(z, b2), d_vec, flag4_vec);    // z>b2 → d, else 4
                z = hn::Max(z, b2);
                z = hn::Min(z, sc_mch_vec);

                // dp_code_block2
                hn::Store(hn::Sub(z, vt1), d_i8, u_ptr + t * VS);
                hn::Store(hn::Sub(z, ut), d_i8, v_ptr + t * VS);
                auto tmp_q  = hn::Sub(z, q_vec);
                a  = hn::Sub(a,  tmp_q);
                b  = hn::Sub(b,  tmp_q);
                auto tmp_q2 = hn::Sub(z, q2_vec);
                a2 = hn::Sub(a2, tmp_q2);
                b2 = hn::Sub(b2, tmp_q2);

                // Right-align: flag set when value >= 0 (opposite of left-align)
                auto zero_gt_a = hn::Gt(zero_vec, a);
                hn::Store(hn::Sub(hn::IfThenZeroElse(zero_gt_a, a), qe_vec), d_i8, x_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenZeroElse(zero_gt_a, flag08_vec));

                auto zero_gt_b = hn::Gt(zero_vec, b);
                hn::Store(hn::Sub(hn::IfThenZeroElse(zero_gt_b, b), qe_vec), d_i8, y_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenZeroElse(zero_gt_b, flag10_vec));

                auto zero_gt_a2 = hn::Gt(zero_vec, a2);
                hn::Store(hn::Sub(hn::IfThenZeroElse(zero_gt_a2, a2), qe2_vec), d_i8, x2_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenZeroElse(zero_gt_a2, flag20_vec));

                auto zero_gt_b2 = hn::Gt(zero_vec, b2);
                hn::Store(hn::Sub(hn::IfThenZeroElse(zero_gt_b2, b2), qe2_vec), d_i8, y2_ptr + t * VS);
                d_vec = hn::Or(d_vec, hn::IfThenZeroElse(zero_gt_b2, flag40_vec));

                hn::Store(d_vec, d_i8, pr + t * VS);
            }
        }

        // ================================================================
        // Max score tracking and Z-drop
        // ================================================================
        if (!approx_max) {
            int32_t max_H, max_t;
            if (r > 0) {
                // d2 difference: no -qe in H update
                max_H = H[en0] = en0 > 0
                    ? H[en0 - 1] + static_cast<int32_t>(u8[en0])
                    : H[en0] + static_cast<int32_t>(v8[en0]);
                max_t = en0;

                auto max_H_v = hn::Set(d_i32, max_H);
                auto max_t_v = hn::Set(d_i32, max_t);

                const int en1 = st0 + (en0 - st0) / 4 * 4;
                int t = st0;
                for (; t < en1; t += 4) {
                    auto H1 = hn::LoadU(d_i32, &H[t]);
                    HWY_ALIGN int32_t v_arr[4] = {
                        static_cast<int32_t>(v8[t]),
                        static_cast<int32_t>(v8[t + 1]),
                        static_cast<int32_t>(v8[t + 2]),
                        static_cast<int32_t>(v8[t + 3])
                    };
                    auto v_inc = hn::Load(d_i32, v_arr);
                    H1 = hn::Add(H1, v_inc);
                    hn::StoreU(H1, d_i32, &H[t]);

                    auto t_bcast = hn::Set(d_i32, t);
                    auto cmp = hn::Gt(H1, max_H_v);
                    max_H_v = hn::IfThenElse(cmp, H1, max_H_v);
                    max_t_v = hn::IfThenElse(cmp, t_bcast, max_t_v);
                }

                HWY_ALIGN int32_t HH[4], tt[4];
                hn::Store(max_H_v, d_i32, HH);
                hn::Store(max_t_v, d_i32, tt);
                for (int i = 0; i < 4; ++i) {
                    if (max_H < HH[i]) {
                        max_H = HH[i];
                        max_t = tt[i] + i;
                    }
                }
                for (; t < en0; ++t) {
                    H[t] += static_cast<int32_t>(v8[t]);
                    if (H[t] > max_H) {
                        max_H = H[t];
                        max_t = t;
                    }
                }
            } else {
                H[0] = static_cast<int32_t>(v8[0]) - qe;
                max_H = H[0];
                max_t = 0;
            }
            if (en0 == tlen - 1 && H[en0] > ez->mte) {
                ez->mte = H[en0];
                ez->mte_q = r - en0;
            }
            if (r - st0 == qlen - 1 && H[st0] > ez->mqe) {
                ez->mqe = H[st0];
                ez->mqe_t = st0;
            }
            // d2 uses e2 for Z-drop (not e)
            if (ksw_apply_zdrop(ez, 1, max_H, r, max_t, zdrop, e2)) break;
            if (r == qlen + tlen - 2 && en0 == tlen - 1) {
                ez->score = H[tlen - 1];
            }
        } else {
            // Approximate max
            if (r > 0) {
                if (last_H0_t >= st0 && last_H0_t <= en0
                    && last_H0_t + 1 >= st0 && last_H0_t + 1 <= en0) {
                    int32_t d0 = v8[last_H0_t];
                    int32_t d1 = u8[last_H0_t + 1];
                    if (d0 > d1) H0 += d0;
                    else { H0 += d1; ++last_H0_t; }
                } else if (last_H0_t >= st0 && last_H0_t <= en0) {
                    H0 += static_cast<int32_t>(v8[last_H0_t]);
                } else {
                    ++last_H0_t;
                    H0 += static_cast<int32_t>(u8[last_H0_t]);
                }
            } else {
                H0 = static_cast<int32_t>(v8[0]) - qe;
                last_H0_t = 0;
            }
            if ((flag & KSW_EZ_APPROX_DROP)
                && ksw_apply_zdrop(ez, 1, H0, r, last_H0_t, zdrop, e2))
                break;
            if (r == qlen + tlen - 2 && en0 == tlen - 1) {
                ez->score = H0;
            }
        }
        last_st = st;
        last_en = en;
    }

    // ========================================================================
    // Backtrack
    // ========================================================================
    if (with_cigar) {
        const int rev_cigar = !!(flag & KSW_EZ_REV_CIGAR);
        if (!ez->zdropped && !(flag & KSW_EZ_EXTZ_ONLY)) {
            ksw_backtrack(km, 1, rev_cigar, 0,
                reinterpret_cast<uint8_t*>(p_ptr), off, off_end,
                n_col_ * 16, tlen - 1, qlen - 1,
                &ez->m_cigar, &ez->n_cigar, &ez->cigar);
        } else if (!ez->zdropped && (flag & KSW_EZ_EXTZ_ONLY)
                   && ez->mqe + end_bonus > static_cast<int>(ez->max)) {
            ez->reach_end = 1;
            ksw_backtrack(km, 1, rev_cigar, 0,
                reinterpret_cast<uint8_t*>(p_ptr), off, off_end,
                n_col_ * 16, ez->mqe_t, qlen - 1,
                &ez->m_cigar, &ez->n_cigar, &ez->cigar);
        } else if (ez->max_t >= 0 && ez->max_q >= 0) {
            ksw_backtrack(km, 1, rev_cigar, 0,
                reinterpret_cast<uint8_t*>(p_ptr), off, off_end,
                n_col_ * 16, ez->max_t, ez->max_q,
                &ez->m_cigar, &ez->n_cigar, &ez->cigar);
        }
    }
}

// ============================================================================
// HwyKsw2Aligner — public API matching the Aligner<ALIGN_ALGORITHM_KSW2> interface
// ============================================================================

/**
 * @brief Highway-based KSW2 alignment dispatcher.
 *
 * Drop-in replacement for Aligner<ALIGN_ALGORITHM_KSW2>. Uses Google Highway
 * for portable SIMD instead of raw SSE/NEON intrinsics.
 *
 * Currently implements z2 (single affine gap) and d2 (dual affine gap).
 * The splice-aware s2 variant is not yet ported.
 */
class HwyKsw2Aligner {
public:
    /**
     * @brief Single-affine-gap extension alignment.
     *
     * Identical signature to Aligner<ALIGN_ALGORITHM_KSW2>::z2().
     */
    static void z2(
        void* km, int qlen, const uint8_t* query,
        int tlen, const uint8_t* target,
        int8_t m, const int8_t* mat,
        int8_t q, int8_t e, int w, int zdrop,
        int end_bonus, int flag, ksw_extz_t* ez)
    {
        hwy_align_z2(km, qlen, query, tlen, target, m, mat,
                      q, e, w, zdrop, end_bonus, flag, ez);
    }

    /**
     * @brief Dual-affine-gap extension alignment.
     *
     * Identical signature to Aligner<ALIGN_ALGORITHM_KSW2>::d2().
     */
    static void d2(
        void* km, int qlen, const uint8_t* query,
        int tlen, const uint8_t* target,
        int8_t m, const int8_t* mat,
        int8_t q, int8_t e, int8_t q2, int8_t e2,
        int w, int zdrop, int end_bonus, int flag,
        ksw_extz_t* ez)
    {
        hwy_align_d2(km, qlen, query, tlen, target, m, mat,
                      q, e, q2, e2, w, zdrop, end_bonus, flag, ez);
    }
};

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
