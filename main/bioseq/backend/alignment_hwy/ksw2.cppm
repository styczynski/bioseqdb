/**
 * @file ksw2.cppm
 * @brief Highway (HWY) SIMD port of the KSW2 extension alignment algorithms.
 *
 * This module provides portable SIMD implementations of the KSW2 extension
 * alignment (Suzuki-Kasahara diagonal formulation) originally written with
 * raw SSE2/SSE4.1 intrinsics. By using Google Highway the code compiles and
 * runs on x86 (SSE2 through AVX-512), ARM NEON/SVE, WASM, and any future
 * target Highway supports.
 *
 * Exported entry point: `HwyKsw2Aligner` with static methods `z2_align()`
 * and `d2_align()`. These are templated by the KSW2 alphabet dimension
 * (`AlphabetDim = alphabet_size + 1`) and accept either
 * `std::span<const uint8_t>` views or `compact_sequence_range<R>` ranges
 * directly; the latter is decoded into temporary byte buffers before the
 * DP kernel runs.
 *
 * The algorithms operate in anti-diagonal (rotated) coordinates with banded
 * dynamic programming. All SIMD vectors are sized by `hn::ScalableTag<int8_t>`
 * capped at 16 lanes to guarantee bit-exact results matching the original
 * SSE implementation on any currently-supported target.
 *
 * Implemented variants:
 *   - `z2`: single-affine-gap extension alignment (`ksw2_extz2_sse` port)
 *   - `d2`: dual-affine-gap extension alignment (`ksw2_extd2_sse` port)
 *
 * Naming in this file follows Suzuki's paper notation as documented in
 * `RESEARCH/ksw/main.tex` and `RESEARCH/MINIMAP2_DETAILS_1.md`:
 *
 *   - `anti_diagonal`       : `r` — anti-diagonal index, r = i + j
 *   - `target_position`     : `t` — target coordinate within an anti-diagonal
 *   - `diff_up[t]`          : `u` — H[i,j] - H[i-1,j] + q + e (non-negative)
 *   - `diff_left[t]`        : `v` — H[i,j] - H[i,j-1] + q + e (non-negative)
 *   - `gap_e_offset[t]`     : `x` — E'[i,j] - H[i,j] + q + e (first gap state)
 *   - `gap_f_offset[t]`     : `y` — F'[i,j] - H[i,j] + q + e (first gap state)
 *   - `gap_e2_offset[t]`    : `x2` (d2 only) — long gap state E-tilde
 *   - `gap_f2_offset[t]`    : `y2` (d2 only) — long gap state F-tilde
 *   - `score_max_cell`      : `z` — per-cell maximum score in the DP update
 *   - `diag_chain_sum`      : `a` — `xt1 + vt1` (carry from prior anti-diag)
 *   - `vert_chain_sum`      : `b` — `y[t] + u[t]` (within current anti-diag)
 *   - `diag_score_band`     : `s` — S(i,j) packed for this anti-diagonal
 *   - `target_bytes`        : `sf` — copy of target rank sequence
 *   - `query_reversed`      : `qr` — reversed query rank sequence
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

// NOTE: this module is intentionally self-contained — it deliberately does NOT
// import the legacy `alignment` module. Everything the KSW2 port needs (result
// struct, flag bitfield, Z-drop helper, CIGAR traceback) is defined below so
// downstream callers can drop the `alignment` dependency as well.
import mlogging;
import sequan_compat;
import bioseq_sequences;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// Self-contained KSW2 constants and CIGAR opcodes.
// -----------------------------------------------------------------------------
// Ported from the legacy `alignment` module so `ksw2.cppm` has no cross-module
// coupling. `kKswNegInf` matches the `KSW_NEG_INF` sentinel the original KSW2
// C code used; the CIGAR opcodes mirror the lowest four bits that the backtrack
// writer emits (M/I/D/N).
// =============================================================================

constexpr int32_t  kKswNegInf      = -0x40000000;
constexpr uint32_t kKswCigarMatch  = 0;  ///< `M` operation (match or mismatch)
constexpr uint32_t kKswCigarIns    = 1;  ///< `I` operation (insertion into reference)
constexpr uint32_t kKswCigarDel    = 2;  ///< `D` operation (deletion from reference)
constexpr uint32_t kKswCigarNSkip  = 3;  ///< `N` operation (skipped reference region / intron)

// =============================================================================
// SIMD tag selection — byte lanes for the DP kernels.
// -----------------------------------------------------------------------------
// We use `hn::ScalableTag<int8_t>` to satisfy the goal of portable,
// target-native vector widths. On every platform we currently target
// (SSE2/SSE4/AVX2/AVX-512 on x86, NEON on ARM, SVE with 128-bit vectors),
// this produces at most 16 8-bit lanes, which matches the block semantics
// of the original SSE port and keeps the DP output bit-identical.
//
// The static assertion below guards against silently widening to a different
// block size — if a target ever exposes more than 16 lanes here, the band
// rounding and the backtrack matrix layout would need to be re-validated
// before the algorithm can be allowed to run unchanged.
// =============================================================================

using KswByteTag   = hn::ScalableTag<int8_t>;
using KswByteTagU  = hn::ScalableTag<uint8_t>;
using KswScoreTag  = hn::Full128<int32_t>;  // exact-max inner loop (4 lanes)

inline constexpr int kKswVecBytes = static_cast<int>(hn::MaxLanes(KswByteTag{}));
inline constexpr int kKswScoreLanes = static_cast<int>(hn::MaxLanes(KswScoreTag{}));

static_assert(kKswVecBytes == 16,
    "KSW2 bit-exact output is validated only for 16-byte SIMD vectors. "
    "Wider SIMD targets need validation before this assertion is relaxed.");
static_assert(kKswScoreLanes == 4,
    "KSW2 exact-max inner loop processes 4 int32 lanes per step; widening "
    "KswScoreTag would require re-validating the score-block max extraction.");

using KswByteVec   = hn::Vec<KswByteTag>;
using KswScoreVec  = hn::Vec<KswScoreTag>;

// =============================================================================
// Small helpers — unsigned 8-bit min/max on int8_t-typed vectors.
// =============================================================================

/**
 * @brief Unsigned 8-bit maximum on int8_t-typed vectors.
 *
 * SSE equivalent: `_mm_max_epu8(a, b)`. Used by the `z2` variant where the
 * score bias keeps all cell values non-negative, requiring unsigned compare.
 */
[[always_inline]] inline KswByteVec max_u8(KswByteTag byte_tag,
                                            KswByteVec left, KswByteVec right) noexcept {
    KswByteTagU unsigned_tag;
    return hn::BitCast(byte_tag,
        hn::Max(hn::BitCast(unsigned_tag, left),
                hn::BitCast(unsigned_tag, right)));
}

/**
 * @brief Unsigned 8-bit minimum on int8_t-typed vectors.
 *
 * SSE equivalent: `_mm_min_epu8(a, b)`.
 */
[[always_inline]] inline KswByteVec min_u8(KswByteTag byte_tag,
                                            KswByteVec left, KswByteVec right) noexcept {
    KswByteTagU unsigned_tag;
    return hn::BitCast(byte_tag,
        hn::Min(hn::BitCast(unsigned_tag, left),
                hn::BitCast(unsigned_tag, right)));
}

/**
 * @brief Create a vector with `value` in lane 0 and zeros everywhere else.
 *
 * SSE equivalent: `_mm_cvtsi32_si128((uint8_t)value)`. Used to seed the
 * stripe-propagation carry at the boundary of each anti-diagonal.
 */
[[always_inline]] inline KswByteVec set_lane_zero(KswByteTag byte_tag,
                                                   int8_t value) noexcept {
    HWY_ALIGN int8_t buffer[kKswVecBytes] = {};
    buffer[0] = value;
    return hn::Load(byte_tag, buffer);
}

/**
 * @brief Shift a vector left by one byte and insert `lane0` at position 0.
 *
 * Carries the last byte (lane `kKswVecBytes - 1`) out through the `lane0`
 * reference so the caller can chain it into the next iteration. This is the
 * standard anti-diagonal stripe-propagation pattern:
 *   result = (vec << 1 byte) | lane0
 *   lane0  = extract last byte of vec
 */
[[always_inline]] inline KswByteVec shift_left_insert(KswByteTag byte_tag,
                                                       KswByteVec vec,
                                                       KswByteVec& lane0) noexcept {
    auto carry_out = hn::ShiftRightBytes<kKswVecBytes - 1>(byte_tag, vec);
    auto shifted   = hn::Or(hn::ShiftLeftBytes<1>(byte_tag, vec), lane0);
    lane0 = carry_out;
    return shifted;
}

// =============================================================================
// Scoring matrix — templated std::array wrapper.
// -----------------------------------------------------------------------------
// The KSW2 `m` parameter equals the alphabet dimension including the
// wildcard row/column: `AlphabetDim = alphabet_size + 1`. The matrix is
// stored row-major with `AlphabetDim * AlphabetDim` int8 entries.
// =============================================================================

/**
 * @brief Compile-time sized scoring matrix for KSW2.
 *
 * The KSW2 DP uses a square scoring matrix indexed by alphabet ranks.
 * `AlphabetDim` is the dimension of the matrix, which for a typical DNA
 * workload is `alphabet_size + 1` (one extra row/column for the wildcard).
 */
template<int AlphabetDim>
struct KswScoringMatrix {
    static_assert(AlphabetDim > 1, "KSW2 requires an alphabet dimension > 1.");

    /** Total number of entries in the matrix. */
    static constexpr size_t entry_count = static_cast<size_t>(AlphabetDim) * AlphabetDim;
    /** Matrix dimension (KSW2 `m` parameter). */
    static constexpr int dim = AlphabetDim;

    /** Row-major score entries, `AlphabetDim * AlphabetDim` in total. */
    std::array<int8_t, entry_count> entries{};

    /** Retrieve the score for `(row, col)` without bounds checking. */
    [[nodiscard]] constexpr int8_t at(int row, int col) const noexcept {
        return entries[static_cast<size_t>(row) * AlphabetDim + col];
    }

    /** Mutable accessor used by score-matrix builders. */
    [[nodiscard]] constexpr int8_t& at(int row, int col) noexcept {
        return entries[static_cast<size_t>(row) * AlphabetDim + col];
    }

    /** Convenience accessor for the canonical match score (diagonal, top-left). */
    [[nodiscard]] constexpr int8_t match_score() const noexcept {
        return entries[0];
    }

    /** Convenience accessor for the canonical mismatch score (first off-diagonal). */
    [[nodiscard]] constexpr int8_t mismatch_score() const noexcept {
        return entries[1];
    }

    /** Last matrix cell — the "N x N" wildcard-vs-wildcard score. */
    [[nodiscard]] constexpr int8_t wildcard_score() const noexcept {
        return entries[entry_count - 1];
    }
};

// =============================================================================
// Flag bitfield — replaces the `flags & KSW_EZ_*` pattern with named booleans.
// -----------------------------------------------------------------------------
// The legacy KSW2 API packs its behaviour switches into a single `int` and
// then tests individual bits via `(flags & KSW_EZ_SCORE_ONLY)` etc. Following
// the bit-field layout used by `MinimapSketchData`, we expose each switch as
// its own one-bit member so call sites read as
// `params.flags.score_only` / `params.flags.right_align` / ... — no bitmask
// arithmetic at the use sites and no risk of flag-bit typos.
//
// The splice-aware `s2` flags are intentionally omitted because the `s2`
// variant is not yet ported to Highway.
// =============================================================================

/**
 * @brief Per-alignment behaviour flags for the KSW2 kernels.
 *
 * One bit per toggle, initialised to `false`. Designated-initialiser
 * construction at call sites gives the clearest semantics, e.g.:
 * `.flags = {.extz_only = true, .right_align = true, .reverse_cigar = true}`.
 */
struct KswFlags {
    bool score_only    : 1 = false;  ///< skip CIGAR computation (equivalent `KSW_EZ_SCORE_ONLY`)
    bool right_align   : 1 = false;  ///< right-align gaps on ties (`KSW_EZ_RIGHT`)
    bool generic_score : 1 = false;  ///< use the full `m*m` scoring matrix (`KSW_EZ_GENERIC_SC`)
    bool approx_max    : 1 = false;  ///< approximate max tracking (`KSW_EZ_APPROX_MAX`)
    bool approx_zdrop  : 1 = false;  ///< approximate Z-drop check (`KSW_EZ_APPROX_DROP`)
    bool extz_only     : 1 = false;  ///< extension-only mode (`KSW_EZ_EXTZ_ONLY`)
    bool reverse_cigar : 1 = false;  ///< output CIGAR already reversed (`KSW_EZ_REV_CIGAR`)
    bool reserved      : 1 = false;  ///< padding to round out to a full byte
};
static_assert(sizeof(KswFlags) == 1,
    "KswFlags should fit into a single byte so Params structs stay compact.");

// =============================================================================
// Parameters — grouped penalty/flag structs for readability.
// =============================================================================

/**
 * @brief Parameter bundle for single-affine-gap alignment (`z2` variant).
 *
 * Gap cost model: `cost(k) = gap_open + k * gap_extend` for a gap of length k.
 * Use designated-initializer construction at call sites, e.g.:
 * `KswZ2Params{.gap_open = 2, .gap_extend = 1, .band_width = 500}`.
 */
struct KswZ2Params {
    int8_t    gap_open   = 0;   ///< `q` — gap open penalty (positive)
    int8_t    gap_extend = 0;   ///< `e` — gap extension penalty (positive)
    int       band_width = -1;  ///< `w` — band half-width, negative disables banding
    int       z_drop     = -1;  ///< z-drop threshold, negative disables
    int       end_bonus  = 0;   ///< bonus for reaching query end in extension mode
    KswFlags  flags      = {};  ///< behaviour toggles
};

/**
 * @brief Parameter bundle for dual-affine-gap alignment (`d2` variant).
 *
 * Dual gap model: `cost(k) = min(q + k*e, q2 + k*e2)`, which lets short gaps
 * use the first linear penalty and long gaps use the second. Use designated-
 * initializer construction at call sites for readability.
 */
struct KswD2Params {
    int8_t    gap_open_short   = 0;   ///< `q` — short-gap open penalty
    int8_t    gap_extend_short = 0;   ///< `e` — short-gap extension penalty
    int8_t    gap_open_long    = 0;   ///< `q2` — long-gap open penalty
    int8_t    gap_extend_long  = 0;   ///< `e2` — long-gap extension penalty
    int       band_width       = -1;
    int       z_drop           = -1;
    int       end_bonus        = 0;
    KswFlags  flags            = {};
};

// =============================================================================
// Sequence view — thin wrapper around a byte range used by the DP kernels.
// =============================================================================

/**
 * @brief Non-owning view over an encoded sequence (ranks as uint8_t).
 *
 * The DP kernels randomly access query/target positions within an anti-
 * diagonal and therefore need a linear byte buffer. This view abstracts the
 * legacy `(const uint8_t*, int length)` pair into a lightweight struct.
 * `length` is an `int32_t` so it interoperates cleanly with the signed
 * coordinates carried by `KswAlignResult`.
 */
struct KswSequenceView {
    const uint8_t* bytes = nullptr;  ///< pointer to first rank byte (may be null if empty)
    int32_t        length = 0;       ///< number of valid ranks

    /** Construct an empty view. */
    constexpr KswSequenceView() noexcept = default;

    /** Construct from a raw pointer + length (e.g. a slice of a larger buffer). */
    constexpr KswSequenceView(const uint8_t* data, int32_t len) noexcept
        : bytes(data), length(len) {}

    /** Construct from a `std::span<const uint8_t>`. */
    explicit constexpr KswSequenceView(std::span<const uint8_t> span) noexcept
        : bytes(span.data()), length(static_cast<int32_t>(span.size())) {}

    /** Construct from a `std::vector<uint8_t>`. */
    explicit KswSequenceView(const std::vector<uint8_t>& vec) noexcept
        : bytes(vec.data()), length(static_cast<int32_t>(vec.size())) {}

    /** `true` when the view has no bases. */
    [[nodiscard]] constexpr bool empty() const noexcept { return length <= 0; }

    /** Convert to a `std::span<const uint8_t>` for range-based APIs. */
    [[nodiscard]] constexpr std::span<const uint8_t> as_span() const noexcept {
        return {bytes, static_cast<size_t>(length)};
    }
};

/**
 * @brief Decode a compact sequence range into a caller-owned byte buffer and
 *        return a view over the freshly decoded bytes.
 *
 * The byte storage lives in `decoded_output`; the returned view points into
 * that vector. Callers retain ownership of the vector so the view remains
 * valid for the duration of the surrounding scope.
 */
template<supports_compact_sequence_range R>
[[nodiscard]] inline KswSequenceView decode_sequence_into(
    const compact_sequence_range<R>& source_sequence,
    std::vector<uint8_t>& decoded_output)
{
    decoded_output = source_sequence.to_encoded_vector();
    return KswSequenceView(decoded_output);
}

// =============================================================================
// KswAlignResult — self-contained replacement for the legacy `ksw_extz_t`.
// -----------------------------------------------------------------------------
// The legacy C struct packed `max:31, zdropped:1` into a word and allocated
// the CIGAR with raw `malloc`/`realloc` behind a `uint32_t*` / capacity pair.
// `KswAlignResult` uses:
//   - a signed 32-bit `max` plus a separate `bool zdropped` (no bit-packing
//     gymnastics and no risk of accidentally sign-extending the flag bit)
//   - `bool reach_end` instead of the int-as-bool `reach_end`
//   - an owning `std::vector<uint32_t>` CIGAR so there is no manual free on
//     any callsite — the former `KswCigarGuard` RAII helper can disappear.
// Each CIGAR entry still encodes `(length << 4) | opcode` to match the
// `kKswCigar*` opcodes above, so downstream CIGAR unpackers don't need to
// learn a new layout.
// =============================================================================

/**
 * @brief KSW2 extension-alignment result.
 *
 * All score fields are signed 32-bit so that max-tracking comparisons in the
 * DP kernels can use native `int32_t` arithmetic without bit-field masking.
 * Sentinel values follow the legacy semantics: `kKswNegInf` for "not yet
 * observed" scores and `-1` for "not yet observed" coordinates.
 */
struct KswAlignResult {
    int32_t max       = 0;            ///< best score observed during the DP
    int32_t score     = kKswNegInf;   ///< score reaching both query/target ends
    int32_t mqe       = kKswNegInf;   ///< best score reaching the query end
    int32_t mte       = kKswNegInf;   ///< best score reaching the target end
    int32_t max_q     = -1;           ///< query coord of `max`
    int32_t max_t     = -1;           ///< target coord of `max`
    int32_t mqe_t     = -1;           ///< target coord of `mqe`
    int32_t mte_q     = -1;           ///< query coord of `mte`
    bool    zdropped  = false;        ///< set when the DP terminated via Z-drop
    bool    reach_end = false;        ///< set when extension reached the query end
    std::vector<uint32_t> cigar;      ///< owned CIGAR, `(length << 4) | opcode` each
};

// =============================================================================
// apply_zdrop_heuristic — inline port of `ksw_apply_zdrop`.
// -----------------------------------------------------------------------------
// Deciding when to abandon a DP extension. Matches the legacy semantics
// byte-for-byte (same comparisons, same update order) so the DP kernels keep
// producing identical coordinates. The result is returned as a `bool` instead
// of an `int` to match modern style; the return value is only meaningful when
// the caller wants to break out of the DP loop.
// =============================================================================

/**
 * @brief Update `result.max`/`max_q`/`max_t` and decide whether to Z-drop.
 *
 * @param result                   In/out: best-score tracking state.
 * @param use_rotated_coords       True when the DP uses anti-diagonal
 *                                 coordinates (`is_rot` in the legacy API).
 * @param current_score            DP cell score to evaluate.
 * @param first_coord              `anti_diagonal` if rotated, `row` otherwise.
 * @param second_coord             `target_position` if rotated, `col` otherwise.
 * @param zdrop_threshold          Z-drop threshold; negative disables the check.
 * @param gap_extension_penalty    Gap-extend used in the distance adjustment.
 * @return `true` if the DP should terminate due to the Z-drop heuristic.
 */
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
        const int target_distance = target_position - result.max_t;
        const int query_distance  = (anti_diagonal - target_position) - result.max_q;
        const int coordinate_difference = target_distance > query_distance
            ? target_distance - query_distance
            : query_distance - target_distance;
        if (zdrop_threshold >= 0
            && result.max - current_score
               > zdrop_threshold + coordinate_difference * gap_extension_penalty)
        {
            result.zdropped = true;
            return true;
        }
    }
    return false;
}

// =============================================================================
// DP arena — owns the aligned scratch storage used by the z2/d2 kernels.
// -----------------------------------------------------------------------------
// The original C implementation interleaves several 16-byte-aligned scratch
// arrays into a single allocation and accesses them through raw byte
// pointers. This wrapper preserves that layout but exposes named accessors
// that return `std::span<int8_t>` / `std::span<uint8_t>` views so the kernel
// bodies can iterate without tangled pointer arithmetic. Two parallel accessor
// families are provided: signed (for `d2`, which uses real signed arithmetic)
// and unsigned (for `z2`, which uses a bias that keeps every cell value in
// the unsigned 8-bit range).
//
// Layout (z2 — 5 state arrays):
//   [diff_up | diff_left | gap_e_offset | gap_f_offset | diag_score]
//   [target_bytes] [query_reversed + 1 slack vector]
//
// Layout (d2 — 7 state arrays):
//   [diff_up | diff_left | gap_e_offset | gap_f_offset |
//    gap_e2_offset | gap_f2_offset | diag_score]
//   [target_bytes] [query_reversed + 1 slack vector]
// =============================================================================

/**
 * @brief Aligned scratch buffer for the DP kernels.
 *
 * The template parameter `StateArrayCount` selects between the `z2` layout
 * (5 per-cell state arrays: u/v/x/y/s) and the `d2` layout (7 arrays adding
 * x2/y2 for the long-gap state).
 */
template<int StateArrayCount>
class KswDpArena {
public:
    /** Bytes per SIMD vector — matches `kKswVecBytes` at the file scope. */
    static constexpr int kVecBytes = kKswVecBytes;

    /**
     * @brief Allocate the arena and zero-initialize every byte.
     *
     * Zero-init is currently load-bearing for both `z2` and `d2`: the
     * score-compute loop only writes cell-score bytes inside
     * `[band_start_raw, band_end_raw]`, while the DP loop's aligned vector
     * loads cover the full rounded `[band_start, band_end]` range. The
     * tails of `target_bytes` and `query_reversed` are also read past the
     * valid sequence length in wide loads. Reading those lanes requires a
     * well-defined value, which we achieve with a single up-front memset.
     */
    KswDpArena(int target_len, int query_len)
        : target_vec_count_(ceil_divide(target_len, kVecBytes))
        , query_vec_count_(ceil_divide(query_len, kVecBytes))
        , total_byte_count_(
            (static_cast<size_t>(StateArrayCount + 1) * target_vec_count_
             + static_cast<size_t>(query_vec_count_) + 1)
            * kVecBytes)
        , storage_(hwy::AllocateAligned<int8_t>(total_byte_count_))
    {
        std::memset(storage_.get(), 0, total_byte_count_);
    }

    /** Overwrite all bytes of state array `array_index` with `value`. */
    void fill_state_bytes(int array_index, uint8_t value) noexcept {
        std::memset(state_bytes_raw(array_index), value,
                    static_cast<size_t>(target_vec_count_) * kVecBytes);
    }

    /**
     * @brief Overwrite state arrays `[0, array_count)` with `value` in a
     *        single `memset` call (hot path for d2's baseline setup).
     */
    void fill_first_state_arrays(int array_count, uint8_t value) noexcept {
        DEBUG_ASSERT(array_count >= 0 && array_count <= StateArrayCount,
            "fill_first_state_arrays would overflow into non-state storage");
        std::memset(state_bytes_raw(0), value,
                    static_cast<size_t>(array_count) * target_vec_count_ * kVecBytes);
    }

    /** Zero-initialize state arrays `[array_index, StateArrayCount)`. */
    void zero_remaining_state_arrays(int array_index) noexcept {
        std::memset(state_bytes_raw(array_index), 0,
                    static_cast<size_t>(StateArrayCount - array_index)
                    * target_vec_count_ * kVecBytes);
    }

    // --- Signed views (used by the d2 kernel) ---------------------------

    /** Signed view of state array `array_index`. */
    [[nodiscard]] std::span<int8_t> state_bytes_signed(int array_index) noexcept {
        return {state_bytes_raw(array_index),
                static_cast<size_t>(target_vec_count_) * kVecBytes};
    }

    // --- Unsigned views (used by the z2 kernel, which biases all scores) --

    /** Unsigned view of state array `array_index`. */
    [[nodiscard]] std::span<uint8_t> state_bytes_unsigned(int array_index) noexcept {
        return {reinterpret_cast<uint8_t*>(state_bytes_raw(array_index)),
                static_cast<size_t>(target_vec_count_) * kVecBytes};
    }

    // --- Target / query byte buffers ------------------------------------

    /**
     * @brief Unsigned view over the target-bytes copy used to compute per-cell
     *        scores. Sized to `target_vec_count * kVecBytes` so the caller can
     *        read past the actual target length within the allocated padding.
     */
    [[nodiscard]] std::span<uint8_t> target_bytes_span() noexcept {
        return {reinterpret_cast<uint8_t*>(
                    storage_.get()
                    + static_cast<size_t>(StateArrayCount) * target_vec_count_ * kVecBytes),
                static_cast<size_t>(target_vec_count_) * kVecBytes};
    }

    /**
     * @brief Unsigned view over the reversed-query bytes. Includes a trailing
     *        slack vector so the scoring loop can load past the end of the
     *        valid query without touching foreign memory.
     */
    [[nodiscard]] std::span<uint8_t> query_reversed_span() noexcept {
        auto target_end = reinterpret_cast<uint8_t*>(
            storage_.get()
            + static_cast<size_t>(StateArrayCount + 1) * target_vec_count_ * kVecBytes);
        return {target_end, static_cast<size_t>(query_vec_count_ + 1) * kVecBytes};
    }

    /** Number of SIMD vectors worth of target state. */
    [[nodiscard]] int target_vec_count() const noexcept { return target_vec_count_; }
    /** Number of SIMD vectors worth of query state. */
    [[nodiscard]] int query_vec_count()  const noexcept { return query_vec_count_; }
    /** Total byte count owned by the arena — useful for assertions/tests. */
    [[nodiscard]] size_t total_byte_count() const noexcept { return total_byte_count_; }

private:
    [[nodiscard]] static constexpr int ceil_divide(int numerator, int divisor) noexcept {
        return (numerator + divisor - 1) / divisor;
    }

    /** Raw first-byte pointer of state array `array_index`. */
    [[nodiscard]] int8_t* state_bytes_raw(int array_index) noexcept {
        return storage_.get()
             + static_cast<size_t>(array_index) * target_vec_count_ * kVecBytes;
    }

    int target_vec_count_;
    int query_vec_count_;
    size_t total_byte_count_;
    hwy::AlignedFreeUniquePtr<int8_t[]> storage_;
};

// =============================================================================
// KswBacktrackMatrix — typed wrapper around the DP backtrack matrix storage.
// -----------------------------------------------------------------------------
// The backtrack matrix is a 2D array indexed by (anti_diagonal, band_vector).
// Each logical "slot" holds one SIMD vector worth of direction/continuation
// flags that the DP kernel writes via `hn::Store` and the traceback reader
// consumes a byte at a time.
//
// We keep the underlying allocation as `hwy::AlignedFreeUniquePtr<int8_t[]>`
// because the backtrack matrix is *write-only* during the DP loop — there is
// no `hn::Load` followed by `hn::Store` for this data, so there is no
// redundant store/load cycle to collapse. Reinterpreting the memory as an
// array of `hn::Vec<KswByteTag>` would still go through `hn::Store` via a
// raw pointer anyway (Highway's API takes element-typed pointers, not
// Vec-typed ones), and `hn::Vec<Tag>` is not guaranteed to be a
// standard-layout type on scalable-vector targets — so the current
// byte-addressed layout is both the most portable *and* the layout that
// yields the minimum number of memory operations.
//
// The wrapper exists to stop callers from hand-rolling
// `base + anti_diagonal * cols * 16 + local_vec * 16` every time they need
// a vector slot: `matrix.vec_slot_bytes(r, local)` computes the same byte
// offset with explicit vector semantics so the pointer arithmetic is
// obviously correct and the "slot stores one vector" invariant is visible
// at call sites. It also keeps the per-anti-diagonal `band_starts` /
// `band_ends` arrays colocated with the matrix they describe.
// =============================================================================

class KswBacktrackMatrix {
public:
    KswBacktrackMatrix() noexcept = default;

    /**
     * @brief Allocate matrix + band-boundary arrays sized for an alignment.
     *
     * @param anti_diagonal_count  Number of anti-diagonals (`qlen + tlen - 1`).
     * @param band_vec_capacity    Number of SIMD vectors per anti-diagonal row.
     *                             The matrix stores `band_vec_capacity` vectors
     *                             for every anti-diagonal, plus one trailing
     *                             slack vector that keeps wide loads in the
     *                             scoring path from touching foreign memory.
     */
    void allocate(int anti_diagonal_count, int band_vec_capacity) {
        DEBUG_ASSERT(anti_diagonal_count > 0,
            "backtrack matrix needs at least one anti-diagonal row", anti_diagonal_count);
        DEBUG_ASSERT(band_vec_capacity > 0,
            "backtrack matrix needs at least one vector per row", band_vec_capacity);
        cols_in_vectors_ = band_vec_capacity;
        byte_cols_per_row_ = band_vec_capacity * kKswVecBytes;
        total_vector_slots_ =
            static_cast<size_t>(anti_diagonal_count) * cols_in_vectors_ + 1;
        const size_t total_bytes = total_vector_slots_ * kKswVecBytes;
        storage_ = hwy::AllocateAligned<int8_t>(total_bytes);
        std::memset(storage_.get(), 0, total_bytes);
        band_starts_.assign(static_cast<size_t>(anti_diagonal_count), 0);
        band_ends_.assign(static_cast<size_t>(anti_diagonal_count), 0);
    }

    /** `true` once `allocate()` has produced real storage. */
    [[nodiscard]] bool is_allocated() const noexcept { return storage_ != nullptr; }

    /**
     * @brief Byte pointer for the `(anti_diagonal, local_band_vec)` slot.
     *
     * `local_band_vec` is `band_vec_index - band_start_vec` — the 0-based
     * vector offset inside the band on this anti-diagonal. The returned
     * pointer is 16-byte aligned (inherited from `AllocateAligned`) so it
     * feeds straight into `hn::Store(vec, tag, ptr)`.
     */
    [[nodiscard]] int8_t* vec_slot_bytes(int anti_diagonal, int local_band_vec) noexcept {
        DEBUG_ASSERT(local_band_vec >= 0 && local_band_vec < cols_in_vectors_,
            "local band vector index out of range", local_band_vec);
        const size_t slot =
            static_cast<size_t>(anti_diagonal) * cols_in_vectors_
            + static_cast<size_t>(local_band_vec);
        DEBUG_ASSERT(slot < total_vector_slots_,
            "backtrack vector slot beyond allocation", slot);
        return storage_.get() + slot * kKswVecBytes;
    }

    /**
     * @brief Byte view used by the scalar CIGAR traceback.
     *
     * The traceback reads single bytes via the
     * `p[r * n_col + (i - off[r])]` pattern, so it needs a flat
     * `const uint8_t*` base. `byte_cols_per_row()` returns the `n_col`
     * multiplier that pairs with it.
     */
    [[nodiscard]] const uint8_t* bytes() const noexcept {
        return reinterpret_cast<const uint8_t*>(storage_.get());
    }

    /** Row stride (in bytes) for the traceback reader's index math. */
    [[nodiscard]] int byte_cols_per_row() const noexcept { return byte_cols_per_row_; }

    /** Mutable band-start array — indexed by anti-diagonal. */
    [[nodiscard]] std::span<int> band_starts() noexcept { return band_starts_; }
    /** Mutable band-end array — indexed by anti-diagonal. */
    [[nodiscard]] std::span<int> band_ends()   noexcept { return band_ends_;   }
    /** Immutable band-start view (for the traceback reader). */
    [[nodiscard]] std::span<const int> band_starts() const noexcept { return band_starts_; }
    /** Immutable band-end view (for the traceback reader). */
    [[nodiscard]] std::span<const int> band_ends()   const noexcept { return band_ends_;   }

private:
    hwy::AlignedFreeUniquePtr<int8_t[]> storage_;
    std::vector<int> band_starts_;
    std::vector<int> band_ends_;
    int    cols_in_vectors_    = 0;
    int    byte_cols_per_row_  = 0;
    size_t total_vector_slots_ = 0;
};

// =============================================================================
// ksw_backtrack_cigar — inline port of the legacy `ksw_backtrack`.
// -----------------------------------------------------------------------------
// Walks the backtrack matrix from `(start_row, start_col)` backwards and
// emits CIGAR operations into `out_cigar`. This is a direct port of
// `ksw_backtrack` from `alignment/defs.hpp`, rewritten around
// `KswBacktrackMatrix` / `std::vector<uint32_t>` and with the continuous
// `off_end` check always enabled (the Highway port always populates both
// bound arrays).
//
// Backtrack byte layout (matches the legacy KSW2 encoding):
//   - bits 0-2: DP state that achieves the cell max
//     0 = H (match/mismatch), 1 = E, 2 = F, 3 = E-tilde, 4 = F-tilde
//   - bit  0x08: continuation on the E state
//   - bit  0x10: continuation on the F state
//   - bit  0x20: continuation on the E-tilde state
//   - bit  0x40: continuation on the F-tilde state
// =============================================================================

/**
 * @brief Emit CIGAR operations for a completed DP by walking the backtrack matrix.
 *
 * @param matrix              Matrix written by the DP kernel — both the byte
 *                            storage and band bounds are read from it.
 * @param use_rotated_coords  True for anti-diagonal coordinate systems (z2/d2).
 * @param output_reversed     If true the CIGAR is left in traceback order;
 *                            otherwise it is reversed to emit from 5' to 3'.
 * @param min_intron_length   Treat long E-tilde runs ≥ this as `N` (splice);
 *                            set to 0 for normal extension/global alignment.
 * @param start_row           Starting row coordinate (target-side).
 * @param start_col           Starting column coordinate (query-side).
 * @param out_cigar           Output CIGAR buffer — cleared before writing.
 */
inline void ksw_backtrack_cigar(
    const KswBacktrackMatrix& matrix,
    bool use_rotated_coords,
    bool output_reversed,
    int min_intron_length,
    int start_row,
    int start_col,
    std::vector<uint32_t>& out_cigar)
{
    DEBUG_ASSERT(matrix.is_allocated(),
        "ksw_backtrack_cigar requires an allocated backtrack matrix", 0);
    out_cigar.clear();

    const uint8_t* backtrack_bytes = matrix.bytes();
    const std::span<const int> band_starts = matrix.band_starts();
    const std::span<const int> band_ends   = matrix.band_ends();
    const int byte_cols_per_row = matrix.byte_cols_per_row();

    auto push_cigar_op = [&](uint32_t opcode, uint32_t length) {
        if (!out_cigar.empty() && (out_cigar.back() & 0xfU) == opcode) {
            out_cigar.back() += length << 4;
        } else {
            out_cigar.push_back((length << 4) | opcode);
        }
    };

    int row_position = start_row;
    int col_position = start_col;
    int traceback_state = 0;

    while (row_position >= 0 && col_position >= 0) {
        int force_state = -1;
        uint32_t backtrack_byte = 0;
        if (use_rotated_coords) {
            const int anti_diagonal = row_position + col_position;
            if (row_position < band_starts[static_cast<size_t>(anti_diagonal)]) {
                force_state = 2;
            }
            if (row_position > band_ends[static_cast<size_t>(anti_diagonal)]) {
                force_state = 1;
            }
            if (force_state < 0) {
                const size_t offset_in_row =
                    static_cast<size_t>(row_position
                        - band_starts[static_cast<size_t>(anti_diagonal)]);
                backtrack_byte = backtrack_bytes[
                    static_cast<size_t>(anti_diagonal) * byte_cols_per_row
                    + offset_in_row];
            }
        } else {
            if (col_position < band_starts[static_cast<size_t>(row_position)]) {
                force_state = 2;
            }
            if (col_position > band_ends[static_cast<size_t>(row_position)]) {
                force_state = 1;
            }
            if (force_state < 0) {
                const size_t offset_in_row =
                    static_cast<size_t>(col_position
                        - band_starts[static_cast<size_t>(row_position)]);
                backtrack_byte = backtrack_bytes[
                    static_cast<size_t>(row_position) * byte_cols_per_row
                    + offset_in_row];
            }
        }

        if (traceback_state == 0) {
            traceback_state = static_cast<int>(backtrack_byte & 7U);
        } else if (!((backtrack_byte >> (traceback_state + 2)) & 1U)) {
            traceback_state = 0;
        }
        if (traceback_state == 0) {
            traceback_state = static_cast<int>(backtrack_byte & 7U);
        }
        if (force_state >= 0) {
            traceback_state = force_state;
        }

        if (traceback_state == 0) {
            push_cigar_op(kKswCigarMatch, 1);
            --row_position;
            --col_position;
        } else if (traceback_state == 1
                   || (traceback_state == 3 && min_intron_length <= 0)) {
            push_cigar_op(kKswCigarDel, 1);
            --row_position;
        } else if (traceback_state == 3 && min_intron_length > 0) {
            push_cigar_op(kKswCigarNSkip, 1);
            --row_position;
        } else {
            push_cigar_op(kKswCigarIns, 1);
            --col_position;
        }
    }

    if (row_position >= 0) {
        const uint32_t skip_op =
            (min_intron_length > 0 && row_position >= min_intron_length)
            ? kKswCigarNSkip : kKswCigarDel;
        push_cigar_op(skip_op, static_cast<uint32_t>(row_position + 1));
    }
    if (col_position >= 0) {
        push_cigar_op(kKswCigarIns, static_cast<uint32_t>(col_position + 1));
    }
    if (!output_reversed) {
        std::reverse(out_cigar.begin(), out_cigar.end());
    }
}

// =============================================================================
// align_z2 — Single-affine-gap banded extension alignment (HWY port)
// =============================================================================

/**
 * @brief Single-affine-gap banded extension alignment using Highway SIMD.
 *
 * Port of `ksw2_extz2_sse`. Operates in anti-diagonal (rotated) coordinates
 * with 8-bit SIMD arithmetic and a score bias that keeps all DP cell values
 * non-negative (so unsigned min/max can drive the core reductions).
 *
 * Gap cost model: `cost(k) = gap_open + k * gap_extend` for a gap of length k.
 *
 * Algorithm outline:
 *   1. Reverse the query and copy the target into aligned scratch arrays.
 *   2. For each anti-diagonal `anti_diagonal ∈ [0, qlen + tlen - 2]`:
 *      a. Compute banded boundaries `[band_start, band_end]` in target space.
 *      b. Seed boundary values for the leftmost/rightmost cells.
 *      c. Compute match/mismatch scores for the band.
 *      d. SIMD DP inner loop — propagate scores through u/v/x/y state arrays.
 *      e. Track the max score and apply the Z-drop heuristic.
 *   3. Backtrack through the DP matrix to produce a CIGAR (when requested).
 *
 * @tparam AlphabetDim  Scoring matrix dimension — `alphabet_size + 1`.
 * @param  query           Query sequence (encoded as rank bytes).
 * @param  target          Target sequence (encoded as rank bytes).
 * @param  scoring_matrix  Compile-time sized scoring matrix.
 * @param  params          Penalty/flag parameters (see `KswZ2Params`).
 * @return Populated `KswAlignResult` — owns its CIGAR vector.
 */
template<int AlphabetDim>
[[nodiscard]] KswAlignResult hwy_align_z2(
    KswSequenceView query,
    KswSequenceView target,
    const KswScoringMatrix<AlphabetDim>& scoring_matrix,
    const KswZ2Params& params)
{
    constexpr int kVecBytes = kKswVecBytes;

    KswByteTag  byte_tag;
    KswScoreTag score_tag;

    KswAlignResult result;
    if (query.empty() || target.empty()) {
        return result;
    }

    const int32_t query_len  = query.length;
    const int32_t target_len = target.length;
    const int8_t gap_open        = params.gap_open;
    const int8_t gap_extend      = params.gap_extend;
    const int    gap_open_plus_extend = gap_open + gap_extend;
    const KswFlags flags         = params.flags;
    const bool   compute_cigar   = !flags.score_only;
    const bool   use_approx_max  = flags.approx_max;

    // --- SIMD constant vectors -------------------------------------------
    const auto zero_vec                = hn::Zero(byte_tag);
    const auto gap_open_vec            = hn::Set(byte_tag, gap_open);
    const auto double_gap_cost_vec     = hn::Set(byte_tag,
        static_cast<int8_t>((gap_open + gap_extend) * 2));
    const auto backtrack_flag_diag_vec = hn::Set(byte_tag, 1);
    const auto backtrack_flag_horz_vec = hn::Set(byte_tag, 2);
    const auto backtrack_flag_e_cont   = hn::Set(byte_tag, 0x08);
    const auto backtrack_flag_f_cont   = hn::Set(byte_tag, 0x10);
    const auto match_score_vec         = hn::Set(byte_tag, scoring_matrix.match_score());
    const auto mismatch_score_vec      = hn::Set(byte_tag, scoring_matrix.mismatch_score());
    const auto wildcard_score_vec      = scoring_matrix.wildcard_score() == 0
        ? hn::Set(byte_tag, static_cast<int8_t>(-gap_extend))
        : hn::Set(byte_tag, scoring_matrix.wildcard_score());
    const auto wildcard_rank_vec       = hn::Set(byte_tag,
        static_cast<int8_t>(AlphabetDim - 1));
    const auto max_plausible_score_vec = hn::Set(byte_tag,
        static_cast<int8_t>(scoring_matrix.match_score() + (gap_open + gap_extend) * 2));

    // --- Band width ------------------------------------------------------
    const int band_width = params.band_width < 0
        ? std::max(target_len, query_len)
        : params.band_width;
    const int left_band  = band_width;
    const int right_band = band_width;

    // --- Score range sanity check ----------------------------------------
    // Only the minimum matters: if it is too negative, the 8-bit bias would
    // overflow. The maximum is checked indirectly through `max_plausible_score_vec`.
    int matrix_min = scoring_matrix.mismatch_score();
    for (size_t entry_index = 1; entry_index < scoring_matrix.entry_count; ++entry_index) {
        const int8_t entry = scoring_matrix.entries[entry_index];
        if (entry < matrix_min) { matrix_min = entry; }
    }
    if (-matrix_min > 2 * gap_open_plus_extend) {
        return result;  // Would overflow the 8-bit bias.
    }

    // --- Block dimensions ------------------------------------------------
    int band_vec_capacity =
        query_len < target_len ? query_len : target_len;
    band_vec_capacity =
        ((band_vec_capacity < band_width + 1 ? band_vec_capacity : band_width + 1)
            + kVecBytes - 1) / kVecBytes + 1;

    // --- Scratch arena (u, v, x, y, s + sf + qr) -------------------------
    // The SIMD load/store path uses the signed `int8_t*` base pointers (the
    // byte tag is `ScalableTag<int8_t>`). z2 carries a score bias that keeps
    // every cell value inside the unsigned 8-bit range, so scalar reads at
    // the anti-diagonal boundary reinterpret the same storage as `uint8_t`.
    KswDpArena<5> arena(target_len, query_len);

    int8_t* diff_up_base      = arena.state_bytes_signed(0).data();
    int8_t* diff_left_base    = arena.state_bytes_signed(1).data();
    int8_t* gap_e_offset_base = arena.state_bytes_signed(2).data();
    int8_t* gap_f_offset_base = arena.state_bytes_signed(3).data();
    int8_t* diag_score_base   = arena.state_bytes_signed(4).data();
    uint8_t* target_bytes     = arena.target_bytes_span().data();
    uint8_t* query_reversed   = arena.query_reversed_span().data();

    // --- Exact max tracking (32-bit score strip) -------------------------
    std::vector<int32_t> exact_score_row_storage;
    int32_t* exact_score_row = nullptr;
    if (!use_approx_max) {
        exact_score_row_storage.assign(
            static_cast<size_t>(arena.target_vec_count()) * kVecBytes, kKswNegInf);
        exact_score_row = exact_score_row_storage.data();
    }

    // --- Backtrack storage ----------------------------------------------
    // The DP loop writes one SIMD vector per (anti_diagonal, local_band_vec)
    // slot; the matrix is read byte-by-byte later by `ksw_backtrack_cigar`.
    KswBacktrackMatrix backtrack;
    std::span<int> anti_diag_band_start{};
    std::span<int> anti_diag_band_end{};
    if (compute_cigar) {
        backtrack.allocate(query_len + target_len - 1, band_vec_capacity);
        anti_diag_band_start = backtrack.band_starts();
        anti_diag_band_end   = backtrack.band_ends();
    }

    // --- Prepare target copy and reversed query --------------------------
    for (int query_index = 0; query_index < query_len; ++query_index) {
        query_reversed[query_index] = query.bytes[query_len - 1 - query_index];
    }
    std::memcpy(target_bytes, target.bytes, static_cast<size_t>(target_len));

    // --- Approximate max tracking ---------------------------------------
    int32_t approx_score = 0;
    int32_t approx_score_target_pos = 0;

    // ====================================================================
    // Main DP loop — one anti-diagonal per iteration
    // ====================================================================
    int prev_band_start = -1;
    int prev_band_end   = -1;
    for (int anti_diagonal = 0; anti_diagonal < query_len + target_len - 1; ++anti_diagonal) {
        int band_start = 0;
        int band_end   = target_len - 1;

        // --- Compute band boundaries (unaligned) -------------------------
        if (band_start < anti_diagonal - query_len + 1) {
            band_start = anti_diagonal - query_len + 1;
        }
        if (band_end > anti_diagonal) {
            band_end = anti_diagonal;
        }
        if (band_start < (anti_diagonal - right_band + 1) >> 1) {
            band_start = (anti_diagonal - right_band + 1) >> 1;
        }
        if (band_end > (anti_diagonal + left_band) >> 1) {
            band_end = (anti_diagonal + left_band) >> 1;
        }
        if (band_start > band_end) {
            result.zdropped = true;
            break;
        }
        const int band_start_raw = band_start;
        const int band_end_raw   = band_end;
        band_start = band_start / kVecBytes * kVecBytes;
        band_end   = (band_end + kVecBytes) / kVecBytes * kVecBytes - 1;

        // --- Boundary conditions for the stripe-propagation carry -------
        // z2 biases every cell so the values stay non-negative; scalar
        // accesses therefore reinterpret the signed arena storage as
        // `uint8_t` for the boundary reads.
        int8_t boundary_x_carry;
        int8_t boundary_v_carry;
        uint8_t* query_reversed_aligned = query_reversed + (query_len - 1 - anti_diagonal);
        uint8_t* diff_up_bytes       = reinterpret_cast<uint8_t*>(diff_up_base);
        uint8_t* diff_left_bytes     = reinterpret_cast<uint8_t*>(diff_left_base);
        uint8_t* gap_e_offset_bytes  = reinterpret_cast<uint8_t*>(gap_e_offset_base);
        uint8_t* gap_f_offset_bytes  = reinterpret_cast<uint8_t*>(gap_f_offset_base);

        if (band_start > 0) {
            if (band_start - 1 >= prev_band_start && band_start - 1 <= prev_band_end) {
                boundary_x_carry = gap_e_offset_bytes[band_start - 1];
                boundary_v_carry = diff_left_bytes[band_start - 1];
            } else {
                boundary_x_carry = 0;
                boundary_v_carry = 0;
            }
        } else {
            boundary_x_carry = 0;
            boundary_v_carry = anti_diagonal ? static_cast<int8_t>(gap_open) : 0;
        }
        if (band_end >= anti_diagonal) {
            DEBUG_ASSERT(anti_diagonal < arena.target_vec_count() * kKswVecBytes,
                "anti_diagonal exceeds target-state byte span — allocation too small");
            gap_f_offset_bytes[anti_diagonal] = 0;
            diff_up_bytes[anti_diagonal] = anti_diagonal
                ? static_cast<uint8_t>(gap_open)
                : 0;
        }

        // --- Compute per-cell match/mismatch scores for this anti-diag --
        if (!flags.generic_score) {
            for (int target_position = band_start_raw;
                 target_position <= band_end_raw;
                 target_position += kVecBytes)
            {
                auto target_lanes = hn::LoadU(byte_tag,
                    reinterpret_cast<const int8_t*>(&target_bytes[target_position]));
                auto query_lanes = hn::LoadU(byte_tag,
                    reinterpret_cast<const int8_t*>(&query_reversed_aligned[target_position]));
                auto wildcard_mask = hn::Or(
                    hn::VecFromMask(byte_tag, hn::Eq(target_lanes, wildcard_rank_vec)),
                    hn::VecFromMask(byte_tag, hn::Eq(query_lanes, wildcard_rank_vec)));
                auto equal_mask = hn::VecFromMask(byte_tag, hn::Eq(target_lanes, query_lanes));
                auto cell_scores = hn::IfThenElse(
                    hn::MaskFromVec(equal_mask), match_score_vec, mismatch_score_vec);
                cell_scores = hn::IfThenElse(
                    hn::MaskFromVec(wildcard_mask), wildcard_score_vec, cell_scores);
                hn::StoreU(cell_scores, byte_tag, diag_score_base + target_position);
            }
        } else {
            for (int target_position = band_start_raw;
                 target_position <= band_end_raw;
                 ++target_position)
            {
                diag_score_base[target_position] = scoring_matrix.at(
                    target_bytes[target_position],
                    query_reversed_aligned[target_position]);
            }
        }

        // --- Initialize boundary vectors for stripe propagation ---------
        auto boundary_x_vec = set_lane_zero(byte_tag, boundary_x_carry);
        auto boundary_v_vec = set_lane_zero(byte_tag, boundary_v_carry);
        const int band_start_vec = band_start / kVecBytes;
        const int band_end_vec   = band_end / kVecBytes;
        DEBUG_ASSERT(band_end_vec - band_start_vec + 1 <= band_vec_capacity,
            "Band width exceeds allocated column count");

        // ================================================================
        // Core DP inner loop over SIMD vectors in the band
        // ================================================================
        if (!compute_cigar) {
            // --- Score-only path (no backtrack directions recorded) -----
            for (int band_vec_index = band_start_vec;
                 band_vec_index <= band_end_vec;
                 ++band_vec_index)
            {
                auto score_max_cell = hn::Add(
                    hn::Load(byte_tag, diag_score_base + band_vec_index * kVecBytes),
                    double_gap_cost_vec);
                auto gap_e_shifted = hn::Load(byte_tag,
                    gap_e_offset_base + band_vec_index * kVecBytes);
                gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
                auto diff_left_shifted = hn::Load(byte_tag,
                    diff_left_base + band_vec_index * kVecBytes);
                diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
                auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
                auto diff_up_cur = hn::Load(byte_tag,
                    diff_up_base + band_vec_index * kVecBytes);
                auto vert_chain_sum = hn::Add(
                    hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);

                // score_max_cell = max(score_max_cell, diag_chain_sum)
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
                // score_max_cell = max(score_max_cell, vert_chain_sum) — unsigned
                score_max_cell = max_u8(byte_tag, score_max_cell, vert_chain_sum);
                score_max_cell = min_u8(byte_tag, score_max_cell, max_plausible_score_vec);

                hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                          diff_up_base + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                          diff_left_base + band_vec_index * kVecBytes);
                score_max_cell = hn::Sub(score_max_cell, gap_open_vec);
                diag_chain_sum = hn::Sub(diag_chain_sum, score_max_cell);
                vert_chain_sum = hn::Sub(vert_chain_sum, score_max_cell);

                hn::Store(hn::Max(diag_chain_sum, zero_vec), byte_tag,
                          gap_e_offset_base + band_vec_index * kVecBytes);
                hn::Store(hn::Max(vert_chain_sum, zero_vec), byte_tag,
                          gap_f_offset_base + band_vec_index * kVecBytes);
            }
        } else if (!flags.right_align) {
            // --- CIGAR path, gap left-alignment -------------------------
            // Each `hn::Store` into the backtrack matrix writes one SIMD
            // vector into slot `(anti_diagonal, band_vec_index - band_start_vec)`.
            // `vec_slot_bytes()` hides the byte math so the "one vector per
            // slot" invariant is visible at the call site; the generated
            // machine code is a single aligned vector store per iteration.
            anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int band_vec_index = band_start_vec;
                 band_vec_index <= band_end_vec;
                 ++band_vec_index)
            {
                auto score_max_cell = hn::Add(
                    hn::Load(byte_tag, diag_score_base + band_vec_index * kVecBytes),
                    double_gap_cost_vec);
                auto gap_e_shifted = hn::Load(byte_tag,
                    gap_e_offset_base + band_vec_index * kVecBytes);
                gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
                auto diff_left_shifted = hn::Load(byte_tag,
                    diff_left_base + band_vec_index * kVecBytes);
                diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
                auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
                auto diff_up_cur = hn::Load(byte_tag,
                    diff_up_base + band_vec_index * kVecBytes);
                auto vert_chain_sum = hn::Add(
                    hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);

                // direction = (diag_chain_sum > score_max_cell) ? 1 : 0
                auto direction_vec = hn::IfThenElseZero(
                    hn::Gt(diag_chain_sum, score_max_cell), backtrack_flag_diag_vec);
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
                // direction |= (vert_chain_sum > score_max_cell) ? 2
                auto gt_vert = hn::Gt(vert_chain_sum, score_max_cell);
                direction_vec = hn::IfThenElse(gt_vert, backtrack_flag_horz_vec, direction_vec);

                score_max_cell = max_u8(byte_tag, score_max_cell, vert_chain_sum);
                score_max_cell = min_u8(byte_tag, score_max_cell, max_plausible_score_vec);
                hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                          diff_up_base + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                          diff_left_base + band_vec_index * kVecBytes);
                score_max_cell = hn::Sub(score_max_cell, gap_open_vec);
                diag_chain_sum = hn::Sub(diag_chain_sum, score_max_cell);
                vert_chain_sum = hn::Sub(vert_chain_sum, score_max_cell);

                auto diag_positive_mask = hn::Gt(diag_chain_sum, zero_vec);
                hn::Store(hn::IfThenElseZero(diag_positive_mask, diag_chain_sum), byte_tag,
                          gap_e_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenElseZero(diag_positive_mask, backtrack_flag_e_cont));

                auto vert_positive_mask = hn::Gt(vert_chain_sum, zero_vec);
                hn::Store(hn::IfThenElseZero(vert_positive_mask, vert_chain_sum), byte_tag,
                          gap_f_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenElseZero(vert_positive_mask, backtrack_flag_f_cont));

                hn::Store(direction_vec, byte_tag,
                    backtrack.vec_slot_bytes(anti_diagonal,
                                             band_vec_index - band_start_vec));
            }
        } else {
            // --- CIGAR path, gap right-alignment ------------------------
            anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int band_vec_index = band_start_vec;
                 band_vec_index <= band_end_vec;
                 ++band_vec_index)
            {
                auto score_max_cell = hn::Add(
                    hn::Load(byte_tag, diag_score_base + band_vec_index * kVecBytes),
                    double_gap_cost_vec);
                auto gap_e_shifted = hn::Load(byte_tag,
                    gap_e_offset_base + band_vec_index * kVecBytes);
                gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
                auto diff_left_shifted = hn::Load(byte_tag,
                    diff_left_base + band_vec_index * kVecBytes);
                diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
                auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
                auto diff_up_cur = hn::Load(byte_tag,
                    diff_up_base + band_vec_index * kVecBytes);
                auto vert_chain_sum = hn::Add(
                    hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);

                // direction = (score_max_cell > diag_chain_sum) ? 0 : 1
                auto direction_vec = hn::IfThenZeroElse(
                    hn::Gt(score_max_cell, diag_chain_sum), backtrack_flag_diag_vec);
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
                // direction = (score_max_cell > vert_chain_sum) ? direction : 2
                auto gt_vert_zero = hn::Gt(score_max_cell, vert_chain_sum);
                direction_vec = hn::IfThenElse(gt_vert_zero, direction_vec, backtrack_flag_horz_vec);

                score_max_cell = max_u8(byte_tag, score_max_cell, vert_chain_sum);
                score_max_cell = min_u8(byte_tag, score_max_cell, max_plausible_score_vec);
                hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                          diff_up_base + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                          diff_left_base + band_vec_index * kVecBytes);
                score_max_cell = hn::Sub(score_max_cell, gap_open_vec);
                diag_chain_sum = hn::Sub(diag_chain_sum, score_max_cell);
                vert_chain_sum = hn::Sub(vert_chain_sum, score_max_cell);

                auto diag_non_positive_mask = hn::Gt(zero_vec, diag_chain_sum);
                hn::Store(hn::IfThenZeroElse(diag_non_positive_mask, diag_chain_sum), byte_tag,
                          gap_e_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenZeroElse(diag_non_positive_mask, backtrack_flag_e_cont));

                auto vert_non_positive_mask = hn::Gt(zero_vec, vert_chain_sum);
                hn::Store(hn::IfThenZeroElse(vert_non_positive_mask, vert_chain_sum), byte_tag,
                          gap_f_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenZeroElse(vert_non_positive_mask, backtrack_flag_f_cont));

                hn::Store(direction_vec, byte_tag,
                    backtrack.vec_slot_bytes(anti_diagonal,
                                             band_vec_index - band_start_vec));
            }
        }

        // ================================================================
        // Max score tracking and Z-drop check
        // ================================================================
        if (!use_approx_max) {
            int32_t anti_diag_max_score;
            int32_t anti_diag_max_target_pos;
            if (anti_diagonal > 0) {
                anti_diag_max_score = exact_score_row[band_end_raw] = band_end_raw > 0
                    ? exact_score_row[band_end_raw - 1]
                      + static_cast<int32_t>(diff_up_bytes[band_end_raw]) - gap_open_plus_extend
                    : exact_score_row[band_end_raw]
                      + static_cast<int32_t>(diff_left_bytes[band_end_raw]) - gap_open_plus_extend;
                anti_diag_max_target_pos = band_end_raw;

                auto max_score_wide_vec  = hn::Set(score_tag, anti_diag_max_score);
                auto max_target_pos_vec  = hn::Set(score_tag, anti_diag_max_target_pos);
                const auto gap_open_plus_extend_vec = hn::Set(score_tag,
                    gap_open_plus_extend);

                const int block_end = band_start_raw + (band_end_raw - band_start_raw) / 4 * 4;
                int target_position = band_start_raw;
                for (; target_position < block_end; target_position += 4) {
                    auto exact_scores = hn::LoadU(score_tag,
                        &exact_score_row[target_position]);
                    HWY_ALIGN int32_t diff_left_scalars[4] = {
                        static_cast<int32_t>(diff_left_bytes[target_position]),
                        static_cast<int32_t>(diff_left_bytes[target_position + 1]),
                        static_cast<int32_t>(diff_left_bytes[target_position + 2]),
                        static_cast<int32_t>(diff_left_bytes[target_position + 3])
                    };
                    auto diff_left_vec = hn::Load(score_tag, diff_left_scalars);
                    exact_scores = hn::Add(exact_scores, diff_left_vec);
                    exact_scores = hn::Sub(exact_scores, gap_open_plus_extend_vec);
                    hn::StoreU(exact_scores, score_tag,
                        &exact_score_row[target_position]);

                    auto target_position_bcast = hn::Set(score_tag, target_position);
                    auto greater_mask = hn::Gt(exact_scores, max_score_wide_vec);
                    max_score_wide_vec = hn::IfThenElse(
                        greater_mask, exact_scores, max_score_wide_vec);
                    max_target_pos_vec = hn::IfThenElse(
                        greater_mask, target_position_bcast, max_target_pos_vec);
                }

                HWY_ALIGN int32_t score_block[4];
                HWY_ALIGN int32_t position_block[4];
                hn::Store(max_score_wide_vec, score_tag, score_block);
                hn::Store(max_target_pos_vec, score_tag, position_block);
                for (int block_lane = 0; block_lane < 4; ++block_lane) {
                    if (anti_diag_max_score < score_block[block_lane]) {
                        anti_diag_max_score = score_block[block_lane];
                        anti_diag_max_target_pos = position_block[block_lane] + block_lane;
                    }
                }
                for (; target_position < band_end_raw; ++target_position) {
                    exact_score_row[target_position] +=
                        static_cast<int32_t>(diff_left_bytes[target_position])
                        - gap_open_plus_extend;
                    if (exact_score_row[target_position] > anti_diag_max_score) {
                        anti_diag_max_score = exact_score_row[target_position];
                        anti_diag_max_target_pos = target_position;
                    }
                }
            } else {
                exact_score_row[0] = static_cast<int32_t>(diff_left_bytes[0])
                    - gap_open_plus_extend - gap_open_plus_extend;
                anti_diag_max_score = exact_score_row[0];
                anti_diag_max_target_pos = 0;
            }
            if (band_end_raw == target_len - 1 && exact_score_row[band_end_raw] > result.mte) {
                result.mte = exact_score_row[band_end_raw];
                result.mte_q = anti_diagonal - band_end_raw;
            }
            if (anti_diagonal - band_start_raw == query_len - 1
                && exact_score_row[band_start_raw] > result.mqe)
            {
                result.mqe = exact_score_row[band_start_raw];
                result.mqe_t = band_start_raw;
            }
            if (apply_zdrop_heuristic(result, /*use_rotated_coords=*/true,
                                      anti_diag_max_score, anti_diagonal,
                                      anti_diag_max_target_pos,
                                      params.z_drop, gap_extend))
            {
                break;
            }
            if (anti_diagonal == query_len + target_len - 2 && band_end_raw == target_len - 1) {
                result.score = exact_score_row[target_len - 1];
            }
        } else {
            // --- Approximate max tracking -----------------------------
            if (anti_diagonal > 0) {
                if (approx_score_target_pos >= band_start_raw
                    && approx_score_target_pos <= band_end_raw
                    && approx_score_target_pos + 1 >= band_start_raw
                    && approx_score_target_pos + 1 <= band_end_raw)
                {
                    const int32_t delta_left = diff_left_bytes[approx_score_target_pos]
                        - gap_open_plus_extend;
                    const int32_t delta_up = diff_up_bytes[approx_score_target_pos + 1]
                        - gap_open_plus_extend;
                    if (delta_left > delta_up) {
                        approx_score += delta_left;
                    } else {
                        approx_score += delta_up;
                        ++approx_score_target_pos;
                    }
                } else if (approx_score_target_pos >= band_start_raw
                           && approx_score_target_pos <= band_end_raw)
                {
                    approx_score += static_cast<int32_t>(diff_left_bytes[approx_score_target_pos])
                        - gap_open_plus_extend;
                } else {
                    ++approx_score_target_pos;
                    approx_score += static_cast<int32_t>(diff_up_bytes[approx_score_target_pos])
                        - gap_open_plus_extend;
                }
                if (flags.approx_zdrop
                    && apply_zdrop_heuristic(result, /*use_rotated_coords=*/true,
                                             approx_score, anti_diagonal,
                                             approx_score_target_pos,
                                             params.z_drop, gap_extend))
                {
                    break;
                }
            } else {
                approx_score = static_cast<int32_t>(diff_left_bytes[0])
                    - gap_open_plus_extend - gap_open_plus_extend;
                approx_score_target_pos = 0;
            }
            if (anti_diagonal == query_len + target_len - 2 && band_end_raw == target_len - 1) {
                result.score = approx_score;
            }
        }
        prev_band_start = band_start;
        prev_band_end   = band_end;
    }

    // ====================================================================
    // Backtrack — produce CIGAR when requested
    // ====================================================================
    if (compute_cigar) {
        const bool output_reversed = flags.reverse_cigar;
        if (!result.zdropped && !flags.extz_only) {
            ksw_backtrack_cigar(backtrack,
                /*use_rotated_coords=*/true, output_reversed,
                /*min_intron_length=*/0,
                target_len - 1, query_len - 1, result.cigar);
        } else if (!result.zdropped && flags.extz_only
                   && result.mqe + params.end_bonus > result.max)
        {
            result.reach_end = true;
            ksw_backtrack_cigar(backtrack,
                /*use_rotated_coords=*/true, output_reversed,
                /*min_intron_length=*/0,
                result.mqe_t, query_len - 1, result.cigar);
        } else if (result.max_t >= 0 && result.max_q >= 0) {
            ksw_backtrack_cigar(backtrack,
                /*use_rotated_coords=*/true, output_reversed,
                /*min_intron_length=*/0,
                result.max_t, result.max_q, result.cigar);
        }
    }

    return result;
}

// =============================================================================
// align_d2 — Dual-affine-gap banded extension alignment (HWY port)
// =============================================================================

/**
 * @brief Dual-affine-gap banded extension alignment using Highway SIMD.
 *
 * Port of `ksw2_extd2_sse`. Uses two affine gap penalty functions combined
 * into `gap_cost(k) = min(q + k*e, q2 + k*e2)`, which captures both short
 * gaps (`q + k*e`) and long gaps (`q2 + k*e2`) efficiently. Two additional
 * DP state arrays (`gap_e2_offset`, `gap_f2_offset`) track the long-gap
 * continuation. All 8-bit arithmetic is signed (no unsigned bias, unlike
 * `z2`).
 *
 * Algorithm structure mirrors `z2` with the following changes:
 *   - Two pairs of gap penalties (swapped if `q + e > q2 + e2`).
 *   - Two additional state arrays for the long-gap state.
 *   - Different score-update formula in the DP code blocks.
 *   - Backtrack encodes five states (`0=H, 1=E, 2=F, 3=E-tilde, 4=F-tilde`).
 *
 * @tparam AlphabetDim  Scoring matrix dimension — `alphabet_size + 1`.
 * @param  query           Query sequence (encoded as rank bytes).
 * @param  target          Target sequence (encoded as rank bytes).
 * @param  scoring_matrix  Compile-time sized scoring matrix.
 * @param  params          Penalty/flag parameters (see `KswD2Params`).
 * @return Populated `KswAlignResult` — owns its CIGAR vector.
 */
template<int AlphabetDim>
[[nodiscard]] KswAlignResult hwy_align_d2(
    KswSequenceView query,
    KswSequenceView target,
    const KswScoringMatrix<AlphabetDim>& scoring_matrix,
    KswD2Params params)
{
    constexpr int kVecBytes = kKswVecBytes;

    KswByteTag  byte_tag;
    KswScoreTag score_tag;

    KswAlignResult result;
    if (query.empty() || target.empty() || AlphabetDim <= 1) {
        return result;
    }

    const int32_t query_len  = query.length;
    const int32_t target_len = target.length;

    // Ensure q+e <= q2+e2 (swap if needed so "short" is really short)
    if (params.gap_open_long + params.gap_extend_long
        < params.gap_open_short + params.gap_extend_short)
    {
        std::swap(params.gap_open_short,   params.gap_open_long);
        std::swap(params.gap_extend_short, params.gap_extend_long);
    }
    const int8_t gap_open_short   = params.gap_open_short;
    const int8_t gap_extend_short = params.gap_extend_short;
    const int8_t gap_open_long    = params.gap_open_long;
    const int8_t gap_extend_long  = params.gap_extend_long;
    const int    gap_open_plus_extend = gap_open_short + gap_extend_short;
    const KswFlags flags         = params.flags;
    const bool   compute_cigar   = !flags.score_only;
    const bool   use_approx_max  = flags.approx_max;

    // --- SIMD constant vectors -------------------------------------------
    const auto zero_vec                = hn::Zero(byte_tag);
    const auto gap_open_short_vec      = hn::Set(byte_tag, gap_open_short);
    const auto gap_open_long_vec       = hn::Set(byte_tag, gap_open_long);
    const auto short_gap_cost_vec      = hn::Set(byte_tag,
        static_cast<int8_t>(gap_open_short + gap_extend_short));
    const auto long_gap_cost_vec       = hn::Set(byte_tag,
        static_cast<int8_t>(gap_open_long + gap_extend_long));
    const auto match_score_vec         = hn::Set(byte_tag, scoring_matrix.match_score());
    const auto mismatch_score_vec      = hn::Set(byte_tag, scoring_matrix.mismatch_score());
    const auto wildcard_score_vec      = scoring_matrix.wildcard_score() == 0
        ? hn::Set(byte_tag, static_cast<int8_t>(-gap_extend_long))
        : hn::Set(byte_tag, scoring_matrix.wildcard_score());
    const auto wildcard_rank_vec       = hn::Set(byte_tag,
        static_cast<int8_t>(AlphabetDim - 1));

    // Backtrack-direction constants (five-state scheme)
    const auto backtrack_dir_1_vec = hn::Set(byte_tag, 1);
    const auto backtrack_dir_2_vec = hn::Set(byte_tag, 2);
    const auto backtrack_dir_3_vec = hn::Set(byte_tag, 3);
    const auto backtrack_dir_4_vec = hn::Set(byte_tag, 4);
    const auto backtrack_e_cont    = hn::Set(byte_tag, 0x08);
    const auto backtrack_f_cont    = hn::Set(byte_tag, 0x10);
    const auto backtrack_e2_cont   = hn::Set(byte_tag, 0x20);
    const auto backtrack_f2_cont   = hn::Set(byte_tag, 0x40);

    // --- Band width ------------------------------------------------------
    const int band_width = params.band_width < 0
        ? std::max(target_len, query_len)
        : params.band_width;
    const int left_band  = band_width;
    const int right_band = band_width;

    // --- Score range sanity check ----------------------------------------
    int matrix_min = scoring_matrix.mismatch_score();
    for (size_t entry_index = 1; entry_index < scoring_matrix.entry_count; ++entry_index) {
        const int8_t entry = scoring_matrix.entries[entry_index];
        if (entry < matrix_min) { matrix_min = entry; }
    }
    if (-matrix_min > 2 * (gap_open_short + gap_extend_short)) {
        return result;
    }

    // --- Long-gap threshold (see Suzuki's paper §3.2) --------------------
    const int long_gap_threshold = gap_extend_short != gap_extend_long
        ? [&]() {
            int threshold = (gap_open_long - gap_open_short)
                / (gap_extend_short - gap_extend_long) - 1;
            if (gap_open_long + gap_extend_long + threshold * gap_extend_long
                > gap_open_short + gap_extend_short + threshold * gap_extend_short)
            {
                ++threshold;
            }
            return threshold;
          }()
        : 0;
    const int long_gap_diff = long_gap_threshold * (gap_extend_short - gap_extend_long)
                              - (gap_open_long - gap_open_short) - gap_extend_long;

    // --- Block dimensions ------------------------------------------------
    int band_vec_capacity =
        query_len < target_len ? query_len : target_len;
    band_vec_capacity =
        ((band_vec_capacity < band_width + 1 ? band_vec_capacity : band_width + 1)
            + kVecBytes - 1) / kVecBytes + 1;

    // --- Scratch arena (u, v, x, y, x2, y2, s + sf + qr) ----------------
    // d2 uses signed arithmetic end-to-end, so the state arrays are accessed
    // as `int8_t*` views throughout. After the arena's zero-init, overwrite
    // the first four state arrays (u, v, x, y) with the short-gap baseline
    // in a single memset, and arrays 4-5 (x2, y2) with the long-gap baseline.
    KswDpArena<7> arena(target_len, query_len);

    arena.fill_first_state_arrays(4,
        static_cast<uint8_t>(-gap_open_short - gap_extend_short));
    arena.fill_state_bytes(4,
        static_cast<uint8_t>(-gap_open_long - gap_extend_long));
    arena.fill_state_bytes(5,
        static_cast<uint8_t>(-gap_open_long - gap_extend_long));

    int8_t* diff_up_base       = arena.state_bytes_signed(0).data();
    int8_t* diff_left_base     = arena.state_bytes_signed(1).data();
    int8_t* gap_e_offset_base  = arena.state_bytes_signed(2).data();
    int8_t* gap_f_offset_base  = arena.state_bytes_signed(3).data();
    int8_t* gap_e2_offset_base = arena.state_bytes_signed(4).data();
    int8_t* gap_f2_offset_base = arena.state_bytes_signed(5).data();
    int8_t* diag_score_base    = arena.state_bytes_signed(6).data();
    uint8_t* target_bytes      = arena.target_bytes_span().data();
    uint8_t* query_reversed    = arena.query_reversed_span().data();

    // --- Exact max tracking ---------------------------------------------
    std::vector<int32_t> exact_score_row_storage;
    int32_t* exact_score_row = nullptr;
    if (!use_approx_max) {
        exact_score_row_storage.assign(
            static_cast<size_t>(arena.target_vec_count()) * kVecBytes, kKswNegInf);
        exact_score_row = exact_score_row_storage.data();
    }

    // --- Backtrack storage ----------------------------------------------
    // See the z2 variant for the rationale behind the byte-addressed
    // matrix; the d2 kernel uses exactly the same write pattern.
    KswBacktrackMatrix backtrack;
    std::span<int> anti_diag_band_start{};
    std::span<int> anti_diag_band_end{};
    if (compute_cigar) {
        backtrack.allocate(query_len + target_len - 1, band_vec_capacity);
        anti_diag_band_start = backtrack.band_starts();
        anti_diag_band_end   = backtrack.band_ends();
    }

    // --- Prepare sequences ----------------------------------------------
    for (int query_index = 0; query_index < query_len; ++query_index) {
        query_reversed[query_index] = query.bytes[query_len - 1 - query_index];
    }
    std::memcpy(target_bytes, target.bytes, static_cast<size_t>(target_len));

    // --- Approximate max tracking ---------------------------------------
    int32_t approx_score = 0;
    int32_t approx_score_target_pos = 0;

    // ====================================================================
    // Main DP loop
    // ====================================================================
    int prev_band_start = -1;
    int prev_band_end   = -1;
    for (int anti_diagonal = 0; anti_diagonal < query_len + target_len - 1; ++anti_diagonal) {
        int band_start = 0;
        int band_end   = target_len - 1;

        // --- Band boundaries ----------------------------------------
        if (band_start < anti_diagonal - query_len + 1) {
            band_start = anti_diagonal - query_len + 1;
        }
        if (band_end > anti_diagonal) {
            band_end = anti_diagonal;
        }
        if (band_start < (anti_diagonal - right_band + 1) >> 1) {
            band_start = (anti_diagonal - right_band + 1) >> 1;
        }
        if (band_end > (anti_diagonal + left_band) >> 1) {
            band_end = (anti_diagonal + left_band) >> 1;
        }
        if (band_start > band_end) {
            result.zdropped = true;
            break;
        }
        const int band_start_raw = band_start;
        const int band_end_raw   = band_end;
        band_start = band_start / kVecBytes * kVecBytes;
        band_end   = (band_end + kVecBytes) / kVecBytes * kVecBytes - 1;

        // --- Boundary conditions (signed 8-bit) ---------------------
        // d2 uses signed arithmetic throughout, so every state array is
        // accessed as `int8_t` — no reinterpret_cast ping-pong.
        int8_t boundary_x_carry;
        int8_t boundary_x2_carry;
        int8_t boundary_v_carry;
        uint8_t* query_reversed_aligned = query_reversed + (query_len - 1 - anti_diagonal);

        if (band_start > 0) {
            if (band_start - 1 >= prev_band_start && band_start - 1 <= prev_band_end) {
                boundary_x_carry  = gap_e_offset_base[band_start - 1];
                boundary_x2_carry = gap_e2_offset_base[band_start - 1];
                boundary_v_carry  = diff_left_base[band_start - 1];
            } else {
                boundary_x_carry  = static_cast<int8_t>(-gap_open_short - gap_extend_short);
                boundary_x2_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
                boundary_v_carry  = static_cast<int8_t>(-gap_open_short - gap_extend_short);
            }
        } else {
            boundary_x_carry  = static_cast<int8_t>(-gap_open_short - gap_extend_short);
            boundary_x2_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
            boundary_v_carry  = anti_diagonal == 0
                ? static_cast<int8_t>(-gap_open_short - gap_extend_short)
                : anti_diagonal < long_gap_threshold
                    ? static_cast<int8_t>(-gap_extend_short)
                    : anti_diagonal == long_gap_threshold
                        ? static_cast<int8_t>(long_gap_diff)
                        : static_cast<int8_t>(-gap_extend_long);
        }
        if (band_end >= anti_diagonal) {
            DEBUG_ASSERT(anti_diagonal < arena.target_vec_count() * kKswVecBytes,
                "anti_diagonal exceeds target-state byte span — allocation too small");
            gap_f_offset_base[anti_diagonal]  =
                static_cast<int8_t>(-gap_open_short - gap_extend_short);
            gap_f2_offset_base[anti_diagonal] =
                static_cast<int8_t>(-gap_open_long - gap_extend_long);
            diff_up_base[anti_diagonal] = anti_diagonal == 0
                ? static_cast<int8_t>(-gap_open_short - gap_extend_short)
                : anti_diagonal < long_gap_threshold
                    ? static_cast<int8_t>(-gap_extend_short)
                    : anti_diagonal == long_gap_threshold
                        ? static_cast<int8_t>(long_gap_diff)
                        : static_cast<int8_t>(-gap_extend_long);
        }

        // --- Compute scores -----------------------------------------
        if (!flags.generic_score) {
            for (int target_position = band_start_raw;
                 target_position <= band_end_raw;
                 target_position += kVecBytes)
            {
                auto target_lanes = hn::LoadU(byte_tag,
                    reinterpret_cast<const int8_t*>(&target_bytes[target_position]));
                auto query_lanes = hn::LoadU(byte_tag,
                    reinterpret_cast<const int8_t*>(&query_reversed_aligned[target_position]));
                auto wildcard_mask = hn::Or(
                    hn::VecFromMask(byte_tag, hn::Eq(target_lanes, wildcard_rank_vec)),
                    hn::VecFromMask(byte_tag, hn::Eq(query_lanes, wildcard_rank_vec)));
                auto equal_mask = hn::VecFromMask(byte_tag, hn::Eq(target_lanes, query_lanes));
                auto cell_scores = hn::IfThenElse(
                    hn::MaskFromVec(equal_mask), match_score_vec, mismatch_score_vec);
                cell_scores = hn::IfThenElse(
                    hn::MaskFromVec(wildcard_mask), wildcard_score_vec, cell_scores);
                hn::StoreU(cell_scores, byte_tag, diag_score_base + target_position);
            }
        } else {
            for (int target_position = band_start_raw;
                 target_position <= band_end_raw;
                 ++target_position)
            {
                diag_score_base[target_position] = scoring_matrix.at(
                    target_bytes[target_position],
                    query_reversed_aligned[target_position]);
            }
        }

        // --- Boundary vectors --------------------------------------
        auto boundary_x_vec  = set_lane_zero(byte_tag, boundary_x_carry);
        auto boundary_x2_vec = set_lane_zero(byte_tag, boundary_x2_carry);
        auto boundary_v_vec  = set_lane_zero(byte_tag, boundary_v_carry);
        const int band_start_vec = band_start / kVecBytes;
        const int band_end_vec   = band_end / kVecBytes;
        DEBUG_ASSERT(band_end_vec - band_start_vec + 1 <= band_vec_capacity,
            "Band width exceeds allocated column count");

        // ============================================================
        // Core DP
        // ============================================================
        if (!compute_cigar) {
            // --- Score-only --------------------------------------
            for (int band_vec_index = band_start_vec;
                 band_vec_index <= band_end_vec;
                 ++band_vec_index)
            {
                auto score_max_cell = hn::Load(byte_tag,
                    diag_score_base + band_vec_index * kVecBytes);
                auto gap_e_shifted = hn::Load(byte_tag,
                    gap_e_offset_base + band_vec_index * kVecBytes);
                gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
                auto diff_left_shifted = hn::Load(byte_tag,
                    diff_left_base + band_vec_index * kVecBytes);
                diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
                auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
                auto diff_up_cur = hn::Load(byte_tag,
                    diff_up_base + band_vec_index * kVecBytes);
                auto vert_chain_sum = hn::Add(
                    hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);

                auto gap_e2_shifted = hn::Load(byte_tag,
                    gap_e2_offset_base + band_vec_index * kVecBytes);
                gap_e2_shifted = shift_left_insert(byte_tag, gap_e2_shifted, boundary_x2_vec);
                auto diag_chain_sum2 = hn::Add(gap_e2_shifted, diff_left_shifted);
                auto vert_chain_sum2 = hn::Add(
                    hn::Load(byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);

                // score_max_cell = max(score, a, b, a2, b2) — all signed
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
                score_max_cell = hn::Max(score_max_cell, vert_chain_sum);
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum2);
                score_max_cell = hn::Max(score_max_cell, vert_chain_sum2);
                score_max_cell = hn::Min(score_max_cell, match_score_vec);

                hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                          diff_up_base + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                          diff_left_base + band_vec_index * kVecBytes);
                auto score_minus_short_open = hn::Sub(score_max_cell, gap_open_short_vec);
                diag_chain_sum  = hn::Sub(diag_chain_sum,  score_minus_short_open);
                vert_chain_sum  = hn::Sub(vert_chain_sum,  score_minus_short_open);
                auto score_minus_long_open = hn::Sub(score_max_cell, gap_open_long_vec);
                diag_chain_sum2 = hn::Sub(diag_chain_sum2, score_minus_long_open);
                vert_chain_sum2 = hn::Sub(vert_chain_sum2, score_minus_long_open);

                hn::Store(hn::Sub(hn::Max(diag_chain_sum,  zero_vec), short_gap_cost_vec),
                          byte_tag, gap_e_offset_base  + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(hn::Max(vert_chain_sum,  zero_vec), short_gap_cost_vec),
                          byte_tag, gap_f_offset_base  + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(hn::Max(diag_chain_sum2, zero_vec), long_gap_cost_vec),
                          byte_tag, gap_e2_offset_base + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(hn::Max(vert_chain_sum2, zero_vec), long_gap_cost_vec),
                          byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes);
            }
        } else if (!flags.right_align) {
            // --- CIGAR, gap left-alignment ----------------------
            anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int band_vec_index = band_start_vec;
                 band_vec_index <= band_end_vec;
                 ++band_vec_index)
            {
                auto score_max_cell = hn::Load(byte_tag,
                    diag_score_base + band_vec_index * kVecBytes);
                auto gap_e_shifted = hn::Load(byte_tag,
                    gap_e_offset_base + band_vec_index * kVecBytes);
                gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
                auto diff_left_shifted = hn::Load(byte_tag,
                    diff_left_base + band_vec_index * kVecBytes);
                diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
                auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
                auto diff_up_cur = hn::Load(byte_tag,
                    diff_up_base + band_vec_index * kVecBytes);
                auto vert_chain_sum = hn::Add(
                    hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);
                auto gap_e2_shifted = hn::Load(byte_tag,
                    gap_e2_offset_base + band_vec_index * kVecBytes);
                gap_e2_shifted = shift_left_insert(byte_tag, gap_e2_shifted, boundary_x2_vec);
                auto diag_chain_sum2 = hn::Add(gap_e2_shifted, diff_left_shifted);
                auto vert_chain_sum2 = hn::Add(
                    hn::Load(byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);

                // Backtrack direction: find which state achieves the max
                auto direction_vec = hn::IfThenElseZero(
                    hn::Gt(diag_chain_sum, score_max_cell), backtrack_dir_1_vec);
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
                direction_vec = hn::IfThenElse(
                    hn::Gt(vert_chain_sum, score_max_cell), backtrack_dir_2_vec, direction_vec);
                score_max_cell = hn::Max(score_max_cell, vert_chain_sum);
                direction_vec = hn::IfThenElse(
                    hn::Gt(diag_chain_sum2, score_max_cell), backtrack_dir_3_vec, direction_vec);
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum2);
                direction_vec = hn::IfThenElse(
                    hn::Gt(vert_chain_sum2, score_max_cell), backtrack_dir_4_vec, direction_vec);
                score_max_cell = hn::Max(score_max_cell, vert_chain_sum2);
                score_max_cell = hn::Min(score_max_cell, match_score_vec);

                hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                          diff_up_base + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                          diff_left_base + band_vec_index * kVecBytes);
                auto score_minus_short_open = hn::Sub(score_max_cell, gap_open_short_vec);
                diag_chain_sum  = hn::Sub(diag_chain_sum,  score_minus_short_open);
                vert_chain_sum  = hn::Sub(vert_chain_sum,  score_minus_short_open);
                auto score_minus_long_open = hn::Sub(score_max_cell, gap_open_long_vec);
                diag_chain_sum2 = hn::Sub(diag_chain_sum2, score_minus_long_open);
                vert_chain_sum2 = hn::Sub(vert_chain_sum2, score_minus_long_open);

                auto diag_positive_mask = hn::Gt(diag_chain_sum, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(diag_positive_mask, diag_chain_sum),
                                   short_gap_cost_vec),
                          byte_tag, gap_e_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenElseZero(diag_positive_mask, backtrack_e_cont));

                auto vert_positive_mask = hn::Gt(vert_chain_sum, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(vert_positive_mask, vert_chain_sum),
                                   short_gap_cost_vec),
                          byte_tag, gap_f_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenElseZero(vert_positive_mask, backtrack_f_cont));

                auto diag2_positive_mask = hn::Gt(diag_chain_sum2, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(diag2_positive_mask, diag_chain_sum2),
                                   long_gap_cost_vec),
                          byte_tag, gap_e2_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenElseZero(diag2_positive_mask, backtrack_e2_cont));

                auto vert2_positive_mask = hn::Gt(vert_chain_sum2, zero_vec);
                hn::Store(hn::Sub(hn::IfThenElseZero(vert2_positive_mask, vert_chain_sum2),
                                   long_gap_cost_vec),
                          byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenElseZero(vert2_positive_mask, backtrack_f2_cont));

                hn::Store(direction_vec, byte_tag,
                    backtrack.vec_slot_bytes(anti_diagonal,
                                             band_vec_index - band_start_vec));
            }
        } else {
            // --- CIGAR, gap right-alignment ---------------------
            anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
            anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
            for (int band_vec_index = band_start_vec;
                 band_vec_index <= band_end_vec;
                 ++band_vec_index)
            {
                auto score_max_cell = hn::Load(byte_tag,
                    diag_score_base + band_vec_index * kVecBytes);
                auto gap_e_shifted = hn::Load(byte_tag,
                    gap_e_offset_base + band_vec_index * kVecBytes);
                gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
                auto diff_left_shifted = hn::Load(byte_tag,
                    diff_left_base + band_vec_index * kVecBytes);
                diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
                auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
                auto diff_up_cur = hn::Load(byte_tag,
                    diff_up_base + band_vec_index * kVecBytes);
                auto vert_chain_sum = hn::Add(
                    hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);
                auto gap_e2_shifted = hn::Load(byte_tag,
                    gap_e2_offset_base + band_vec_index * kVecBytes);
                gap_e2_shifted = shift_left_insert(byte_tag, gap_e2_shifted, boundary_x2_vec);
                auto diag_chain_sum2 = hn::Add(gap_e2_shifted, diff_left_shifted);
                auto vert_chain_sum2 = hn::Add(
                    hn::Load(byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes),
                    diff_up_cur);

                // Right-align: prefer gaps on the right (opposite tie-break)
                auto direction_vec = hn::IfThenZeroElse(
                    hn::Gt(score_max_cell, diag_chain_sum), backtrack_dir_1_vec);
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
                direction_vec = hn::IfThenElse(
                    hn::Gt(score_max_cell, vert_chain_sum), direction_vec, backtrack_dir_2_vec);
                score_max_cell = hn::Max(score_max_cell, vert_chain_sum);
                direction_vec = hn::IfThenElse(
                    hn::Gt(score_max_cell, diag_chain_sum2), direction_vec, backtrack_dir_3_vec);
                score_max_cell = hn::Max(score_max_cell, diag_chain_sum2);
                direction_vec = hn::IfThenElse(
                    hn::Gt(score_max_cell, vert_chain_sum2), direction_vec, backtrack_dir_4_vec);
                score_max_cell = hn::Max(score_max_cell, vert_chain_sum2);
                score_max_cell = hn::Min(score_max_cell, match_score_vec);

                hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                          diff_up_base + band_vec_index * kVecBytes);
                hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                          diff_left_base + band_vec_index * kVecBytes);
                auto score_minus_short_open = hn::Sub(score_max_cell, gap_open_short_vec);
                diag_chain_sum  = hn::Sub(diag_chain_sum,  score_minus_short_open);
                vert_chain_sum  = hn::Sub(vert_chain_sum,  score_minus_short_open);
                auto score_minus_long_open = hn::Sub(score_max_cell, gap_open_long_vec);
                diag_chain_sum2 = hn::Sub(diag_chain_sum2, score_minus_long_open);
                vert_chain_sum2 = hn::Sub(vert_chain_sum2, score_minus_long_open);

                auto diag_non_positive_mask = hn::Gt(zero_vec, diag_chain_sum);
                hn::Store(hn::Sub(hn::IfThenZeroElse(diag_non_positive_mask, diag_chain_sum),
                                   short_gap_cost_vec),
                          byte_tag, gap_e_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenZeroElse(diag_non_positive_mask, backtrack_e_cont));

                auto vert_non_positive_mask = hn::Gt(zero_vec, vert_chain_sum);
                hn::Store(hn::Sub(hn::IfThenZeroElse(vert_non_positive_mask, vert_chain_sum),
                                   short_gap_cost_vec),
                          byte_tag, gap_f_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenZeroElse(vert_non_positive_mask, backtrack_f_cont));

                auto diag2_non_positive_mask = hn::Gt(zero_vec, diag_chain_sum2);
                hn::Store(hn::Sub(hn::IfThenZeroElse(diag2_non_positive_mask, diag_chain_sum2),
                                   long_gap_cost_vec),
                          byte_tag, gap_e2_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenZeroElse(diag2_non_positive_mask, backtrack_e2_cont));

                auto vert2_non_positive_mask = hn::Gt(zero_vec, vert_chain_sum2);
                hn::Store(hn::Sub(hn::IfThenZeroElse(vert2_non_positive_mask, vert_chain_sum2),
                                   long_gap_cost_vec),
                          byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes);
                direction_vec = hn::Or(direction_vec,
                    hn::IfThenZeroElse(vert2_non_positive_mask, backtrack_f2_cont));

                hn::Store(direction_vec, byte_tag,
                    backtrack.vec_slot_bytes(anti_diagonal,
                                             band_vec_index - band_start_vec));
            }
        }

        // ============================================================
        // Max score tracking and Z-drop
        // ============================================================
        if (!use_approx_max) {
            int32_t anti_diag_max_score;
            int32_t anti_diag_max_target_pos;
            if (anti_diagonal > 0) {
                // d2 difference: no -qe in H update
                anti_diag_max_score = exact_score_row[band_end_raw] = band_end_raw > 0
                    ? exact_score_row[band_end_raw - 1] + static_cast<int32_t>(diff_up_base[band_end_raw])
                    : exact_score_row[band_end_raw] + static_cast<int32_t>(diff_left_base[band_end_raw]);
                anti_diag_max_target_pos = band_end_raw;

                auto max_score_wide_vec = hn::Set(score_tag, anti_diag_max_score);
                auto max_target_pos_vec = hn::Set(score_tag, anti_diag_max_target_pos);

                const int block_end = band_start_raw + (band_end_raw - band_start_raw) / 4 * 4;
                int target_position = band_start_raw;
                for (; target_position < block_end; target_position += 4) {
                    auto exact_scores = hn::LoadU(score_tag,
                        &exact_score_row[target_position]);
                    HWY_ALIGN int32_t diff_left_scalars[4] = {
                        static_cast<int32_t>(diff_left_base[target_position]),
                        static_cast<int32_t>(diff_left_base[target_position + 1]),
                        static_cast<int32_t>(diff_left_base[target_position + 2]),
                        static_cast<int32_t>(diff_left_base[target_position + 3])
                    };
                    auto diff_left_vec = hn::Load(score_tag, diff_left_scalars);
                    exact_scores = hn::Add(exact_scores, diff_left_vec);
                    hn::StoreU(exact_scores, score_tag,
                        &exact_score_row[target_position]);

                    auto target_position_bcast = hn::Set(score_tag, target_position);
                    auto greater_mask = hn::Gt(exact_scores, max_score_wide_vec);
                    max_score_wide_vec = hn::IfThenElse(
                        greater_mask, exact_scores, max_score_wide_vec);
                    max_target_pos_vec = hn::IfThenElse(
                        greater_mask, target_position_bcast, max_target_pos_vec);
                }

                HWY_ALIGN int32_t score_block[4];
                HWY_ALIGN int32_t position_block[4];
                hn::Store(max_score_wide_vec, score_tag, score_block);
                hn::Store(max_target_pos_vec, score_tag, position_block);
                for (int block_lane = 0; block_lane < 4; ++block_lane) {
                    if (anti_diag_max_score < score_block[block_lane]) {
                        anti_diag_max_score = score_block[block_lane];
                        anti_diag_max_target_pos = position_block[block_lane] + block_lane;
                    }
                }
                for (; target_position < band_end_raw; ++target_position) {
                    exact_score_row[target_position] +=
                        static_cast<int32_t>(diff_left_base[target_position]);
                    if (exact_score_row[target_position] > anti_diag_max_score) {
                        anti_diag_max_score = exact_score_row[target_position];
                        anti_diag_max_target_pos = target_position;
                    }
                }
            } else {
                exact_score_row[0] = static_cast<int32_t>(diff_left_base[0])
                    - gap_open_plus_extend;
                anti_diag_max_score = exact_score_row[0];
                anti_diag_max_target_pos = 0;
            }
            if (band_end_raw == target_len - 1 && exact_score_row[band_end_raw] > result.mte) {
                result.mte = exact_score_row[band_end_raw];
                result.mte_q = anti_diagonal - band_end_raw;
            }
            if (anti_diagonal - band_start_raw == query_len - 1
                && exact_score_row[band_start_raw] > result.mqe)
            {
                result.mqe = exact_score_row[band_start_raw];
                result.mqe_t = band_start_raw;
            }
            // d2 uses e2 for Z-drop (not e)
            if (apply_zdrop_heuristic(result, /*use_rotated_coords=*/true,
                                      anti_diag_max_score, anti_diagonal,
                                      anti_diag_max_target_pos,
                                      params.z_drop, gap_extend_long))
            {
                break;
            }
            if (anti_diagonal == query_len + target_len - 2 && band_end_raw == target_len - 1) {
                result.score = exact_score_row[target_len - 1];
            }
        } else {
            // --- Approximate max ------------------------------
            if (anti_diagonal > 0) {
                if (approx_score_target_pos >= band_start_raw
                    && approx_score_target_pos <= band_end_raw
                    && approx_score_target_pos + 1 >= band_start_raw
                    && approx_score_target_pos + 1 <= band_end_raw)
                {
                    const int32_t delta_left = diff_left_base[approx_score_target_pos];
                    const int32_t delta_up = diff_up_base[approx_score_target_pos + 1];
                    if (delta_left > delta_up) {
                        approx_score += delta_left;
                    } else {
                        approx_score += delta_up;
                        ++approx_score_target_pos;
                    }
                } else if (approx_score_target_pos >= band_start_raw
                           && approx_score_target_pos <= band_end_raw)
                {
                    approx_score +=
                        static_cast<int32_t>(diff_left_base[approx_score_target_pos]);
                } else {
                    ++approx_score_target_pos;
                    approx_score +=
                        static_cast<int32_t>(diff_up_base[approx_score_target_pos]);
                }
            } else {
                approx_score = static_cast<int32_t>(diff_left_base[0])
                    - gap_open_plus_extend;
                approx_score_target_pos = 0;
            }
            if (flags.approx_zdrop
                && apply_zdrop_heuristic(result, /*use_rotated_coords=*/true,
                                         approx_score, anti_diagonal,
                                         approx_score_target_pos,
                                         params.z_drop, gap_extend_long))
            {
                break;
            }
            if (anti_diagonal == query_len + target_len - 2 && band_end_raw == target_len - 1) {
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
        const bool output_reversed = flags.reverse_cigar;
        if (!result.zdropped && !flags.extz_only) {
            ksw_backtrack_cigar(backtrack,
                /*use_rotated_coords=*/true, output_reversed,
                /*min_intron_length=*/0,
                target_len - 1, query_len - 1, result.cigar);
        } else if (!result.zdropped && flags.extz_only
                   && result.mqe + params.end_bonus > result.max)
        {
            result.reach_end = true;
            ksw_backtrack_cigar(backtrack,
                /*use_rotated_coords=*/true, output_reversed,
                /*min_intron_length=*/0,
                result.mqe_t, query_len - 1, result.cigar);
        } else if (result.max_t >= 0 && result.max_q >= 0) {
            ksw_backtrack_cigar(backtrack,
                /*use_rotated_coords=*/true, output_reversed,
                /*min_intron_length=*/0,
                result.max_t, result.max_q, result.cigar);
        }
    }

    return result;
}

// =============================================================================
// HwyKsw2Aligner — public API.
// -----------------------------------------------------------------------------
// The dispatcher exposes four overloads:
//   - `z2_align(query, target, matrix, params)`            (byte views)
//   - `d2_align(query, target, matrix, params)`            (byte views)
//   - `z2_align(query_range, target_range, matrix, params)`  (compact_sequence_range)
//   - `d2_align(query_range, target_range, matrix, params)`  (compact_sequence_range)
//
// Both range overloads decode their inputs into temporary byte buffers and
// delegate to the byte-view kernels. The core kernels are templated on the
// alphabet dimension so the compiler can inline the scoring matrix reads.
// =============================================================================

/**
 * @brief Highway-based KSW2 alignment dispatcher.
 *
 * Drop-in replacement for `Aligner<ALIGN_ALGORITHM_KSW2>`. Uses Google
 * Highway portable SIMD instead of raw SSE/NEON intrinsics.
 *
 * Currently implements `z2` (single affine gap) and `d2` (dual affine gap).
 * The splice-aware `s2` variant is not yet ported.
 */
class HwyKsw2Aligner {
public:
    /**
     * @brief Single-affine-gap extension alignment (byte views).
     */
    template<int AlphabetDim>
    [[nodiscard]] static KswAlignResult z2_align(
        KswSequenceView query,
        KswSequenceView target,
        const KswScoringMatrix<AlphabetDim>& scoring_matrix,
        const KswZ2Params& params)
    {
        JustLog("[kws2] HwyKsw2Aligner: Call Z2 alignment");
        return hwy_align_z2<AlphabetDim>(query, target, scoring_matrix, params);
    }

    /**
     * @brief Dual-affine-gap extension alignment (byte views).
     */
    template<int AlphabetDim>
    [[nodiscard]] static KswAlignResult d2_align(
        KswSequenceView query,
        KswSequenceView target,
        const KswScoringMatrix<AlphabetDim>& scoring_matrix,
        const KswD2Params& params)
    {
        JustLog("[kws2]  HwyKsw2Aligner: Call D2 alignment");
        return hwy_align_d2<AlphabetDim>(query, target, scoring_matrix, params);
    }

    /**
     * @brief Single-affine-gap alignment accepting `compact_sequence_range`
     *        inputs directly.
     *
     * The `AlphabetDim` template parameter defaults to
     * `alphabet_size + 1` drawn from the query's alphabet traits. The query
     * and target alphabets must match — enforced by a `static_assert`
     * inside the function body — otherwise the scoring matrix would be
     * silently mis-indexed.
     *
     * Note: this overload allocates two temporary `std::vector<uint8_t>`
     * buffers to decode the ranges. For hot-path usage (e.g. inside
     * `minimap_port::align_regions`), prefer the byte-view overload with
     * caller-managed scratch storage.
     */
    template<
        supports_compact_sequence_range QueryRange,
        supports_compact_sequence_range TargetRange,
        int AlphabetDim = static_cast<int>(
            seqan3::alphabet_size<typename compact_sequence_range<QueryRange>::traits::alphabet_type>) + 1>
    [[nodiscard]] static KswAlignResult z2_align(
        const compact_sequence_range<QueryRange>& query,
        const compact_sequence_range<TargetRange>& target,
        const KswScoringMatrix<AlphabetDim>& scoring_matrix,
        const KswZ2Params& params)
    {
        JustLog("[kws2]  HwyKsw2Aligner: Call Z2 alignment (DIRECT)");
        static_assert(std::is_same_v<
            typename compact_sequence_range<QueryRange>::traits::alphabet_type,
            typename compact_sequence_range<TargetRange>::traits::alphabet_type>,
            "KSW2 requires query and target to share the same alphabet type.");
        std::vector<uint8_t> query_bytes;
        std::vector<uint8_t> target_bytes;
        auto query_view  = decode_sequence_into(query,  query_bytes);
        auto target_view = decode_sequence_into(target, target_bytes);
        return hwy_align_z2<AlphabetDim>(query_view, target_view, scoring_matrix, params);
    }

    /**
     * @brief Dual-affine-gap alignment accepting `compact_sequence_range`
     *        inputs directly.
     *
     * Same caveat as `z2_align`: query and target must share an alphabet
     * type, and this overload allocates scratch byte buffers.
     */
    template<
        supports_compact_sequence_range QueryRange,
        supports_compact_sequence_range TargetRange,
        int AlphabetDim = static_cast<int>(
            seqan3::alphabet_size<typename compact_sequence_range<QueryRange>::traits::alphabet_type>) + 1>
    [[nodiscard]] static KswAlignResult d2_align(
        const compact_sequence_range<QueryRange>& query,
        const compact_sequence_range<TargetRange>& target,
        const KswScoringMatrix<AlphabetDim>& scoring_matrix,
        const KswD2Params& params)
    {
        JustLog("[kws2]  HwyKsw2Aligner: Call D2 alignment (DIRECT)");
        static_assert(std::is_same_v<
            typename compact_sequence_range<QueryRange>::traits::alphabet_type,
            typename compact_sequence_range<TargetRange>::traits::alphabet_type>,
            "KSW2 requires query and target to share the same alphabet type.");
        std::vector<uint8_t> query_bytes;
        std::vector<uint8_t> target_bytes;
        auto query_view  = decode_sequence_into(query,  query_bytes);
        auto target_view = decode_sequence_into(target, target_bytes);
        return hwy_align_d2<AlphabetDim>(query_view, target_view, scoring_matrix, params);
    }
};

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
