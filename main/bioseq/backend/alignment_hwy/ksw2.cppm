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

import alignment;
import mlogging;
import sequan_compat;
import bioseq_sequences;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

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
    int8_t gap_open         = 0;   ///< `q` — gap open penalty (positive)
    int8_t gap_extend       = 0;   ///< `e` — gap extension penalty (positive)
    int    band_width       = -1;  ///< `w` — band half-width, negative disables banding
    int    z_drop           = -1;  ///< z-drop threshold, negative disables
    int    end_bonus        = 0;   ///< bonus for reaching query end in extension mode
    int    flags            = 0;   ///< `KSW_EZ_*` flag bitmask
};

/**
 * @brief Parameter bundle for dual-affine-gap alignment (`d2` variant).
 *
 * Dual gap model: `cost(k) = min(q + k*e, q2 + k*e2)`, which lets short gaps
 * use the first linear penalty and long gaps use the second. Use designated-
 * initializer construction at call sites for readability.
 */
struct KswD2Params {
    int8_t gap_open_short   = 0;   ///< `q` — short-gap open penalty
    int8_t gap_extend_short = 0;   ///< `e` — short-gap extension penalty
    int8_t gap_open_long    = 0;   ///< `q2` — long-gap open penalty
    int8_t gap_extend_long  = 0;   ///< `e2` — long-gap extension penalty
    int    band_width       = -1;
    int    z_drop           = -1;
    int    end_bonus        = 0;
    int    flags            = 0;
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
 * `length` is an `int32_t` to interop cleanly with the `ksw_extz_t`
 * coordinates (which are all `int`).
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
// ksw_extz_t cleanup helper — RAII guard for the C-allocated CIGAR buffer.
// =============================================================================

/**
 * @brief RAII helper that `free()`s `ksw_extz_t::cigar` on scope exit.
 *
 * KSW2 allocates the CIGAR via `kmalloc`/`krealloc`; when `memory_pool` is
 * null (our usage), the upstream helper falls back to plain `malloc`/`free`.
 * This guard hides the three-phase cleanup ritual that `align_regions` in
 * `minimap_port` would otherwise need to repeat at every exit path.
 */
class KswCigarGuard {
public:
    explicit KswCigarGuard(ksw_extz_t& result) noexcept : result_(&result) {}

    KswCigarGuard(const KswCigarGuard&)            = delete;
    KswCigarGuard& operator=(const KswCigarGuard&) = delete;
    KswCigarGuard(KswCigarGuard&&)                 = delete;
    KswCigarGuard& operator=(KswCigarGuard&&)      = delete;

    ~KswCigarGuard() noexcept {
        if (result_ && result_->cigar) {
            std::free(result_->cigar);
            result_->cigar = nullptr;
            result_->n_cigar = 0;
            result_->m_cigar = 0;
        }
    }

private:
    ksw_extz_t* result_;
};

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

/**
 * @brief Aligned backtrack matrix + offset arrays for the DP kernels.
 *
 * Only allocated when the caller requests a CIGAR; wraps the three separate
 * allocations that used to live as loose variables inside the kernels.
 */
struct KswBacktrackStorage {
    hwy::AlignedFreeUniquePtr<int8_t[]> matrix_bytes;  ///< backtrack directions
    std::vector<int>                    band_starts;   ///< anti-diagonal band start per r
    std::vector<int>                    band_ends;     ///< anti-diagonal band end per r

    /** Raw pointer to the backtrack matrix bytes (may be null when disabled). */
    [[nodiscard]] int8_t* matrix_ptr() noexcept { return matrix_bytes.get(); }

    /** Pointer view of `band_starts` for the C-style `ksw_backtrack` API. */
    [[nodiscard]] int* band_start_ptr() noexcept { return band_starts.data(); }

    /** Pointer view of `band_ends` for the C-style `ksw_backtrack` API. */
    [[nodiscard]] int* band_end_ptr() noexcept { return band_ends.data(); }
};

/**
 * @brief Allocate backtrack storage for an alignment with `query_len` + `target_len` anti-diagonals.
 */
inline KswBacktrackStorage make_backtrack_storage(int query_len, int target_len,
                                                   int band_vec_capacity, int vec_bytes)
{
    KswBacktrackStorage storage;
    const int anti_diagonal_count = query_len + target_len - 1;
    const size_t matrix_bytes_count =
        (static_cast<size_t>(anti_diagonal_count) * band_vec_capacity + 1)
        * vec_bytes;
    storage.matrix_bytes = hwy::AllocateAligned<int8_t>(matrix_bytes_count);
    std::memset(storage.matrix_bytes.get(), 0, matrix_bytes_count);
    storage.band_starts.assign(static_cast<size_t>(anti_diagonal_count), 0);
    storage.band_ends.assign(static_cast<size_t>(anti_diagonal_count), 0);
    return storage;
}

// =============================================================================
// ksw_extz_t helpers.
// =============================================================================

/**
 * @brief Produce a zeroed `ksw_extz_t` via the upstream reset helper.
 *
 * `ksw_reset_extz` only rewrites the fields it cares about, so we start
 * from a value-initialized struct to make sure the `cigar` pointer and any
 * padding are zero. The `{}` initializer is sufficient — no explicit
 * memset required.
 */
[[nodiscard]] inline ksw_extz_t make_empty_extz() noexcept {
    ksw_extz_t result{};
    ksw_reset_extz(&result);
    return result;
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
 * @return Populated `ksw_extz_t` result.
 */
template<int AlphabetDim>
[[nodiscard]] ksw_extz_t hwy_align_z2(
    KswSequenceView query,
    KswSequenceView target,
    const KswScoringMatrix<AlphabetDim>& scoring_matrix,
    const KswZ2Params& params)
{
    constexpr int kVecBytes = kKswVecBytes;

    KswByteTag  byte_tag;
    KswScoreTag score_tag;

    ksw_extz_t result = make_empty_extz();
    if (query.empty() || target.empty()) {
        return result;
    }

    const int32_t query_len  = query.length;
    const int32_t target_len = target.length;
    const int8_t gap_open        = params.gap_open;
    const int8_t gap_extend      = params.gap_extend;
    const int    gap_open_plus_extend = gap_open + gap_extend;
    const int    raw_flags       = params.flags;
    const bool   compute_cigar   = (raw_flags & KSW_EZ_SCORE_ONLY) == 0;
    const bool   use_approx_max  = (raw_flags & KSW_EZ_APPROX_MAX) != 0;

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
            static_cast<size_t>(arena.target_vec_count()) * kVecBytes, KSW_NEG_INF);
        exact_score_row = exact_score_row_storage.data();
    }

    // --- Backtrack storage ----------------------------------------------
    KswBacktrackStorage backtrack;
    int8_t* backtrack_matrix = nullptr;
    int*    anti_diag_band_start_ptr = nullptr;
    int*    anti_diag_band_end_ptr   = nullptr;
    if (compute_cigar) {
        backtrack = make_backtrack_storage(query_len, target_len,
                                           band_vec_capacity, kVecBytes);
        backtrack_matrix         = backtrack.matrix_ptr();
        anti_diag_band_start_ptr = backtrack.band_start_ptr();
        anti_diag_band_end_ptr   = backtrack.band_end_ptr();
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
            result.zdropped = 1;
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
        if (!(raw_flags & KSW_EZ_GENERIC_SC)) {
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
        } else if (!(raw_flags & KSW_EZ_RIGHT)) {
            // --- CIGAR path, gap left-alignment -------------------------
            // The backtrack matrix row for this anti-diagonal starts at
            // `anti_diagonal * band_vec_capacity * kVecBytes`. Per-vector
            // writes land at `(band_vec_index - band_start_vec)` within
            // that row; we keep the subtraction local so the intermediate
            // pointer never goes below the allocation base.
            const size_t anti_diag_row_base =
                static_cast<size_t>(anti_diagonal) * band_vec_capacity * kVecBytes;
            anti_diag_band_start_ptr[anti_diagonal] = band_start;
            anti_diag_band_end_ptr[anti_diagonal] = band_end;
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
                    backtrack_matrix + anti_diag_row_base
                    + static_cast<size_t>(band_vec_index - band_start_vec) * kVecBytes);
            }
        } else {
            // --- CIGAR path, gap right-alignment ------------------------
            const size_t anti_diag_row_base =
                static_cast<size_t>(anti_diagonal) * band_vec_capacity * kVecBytes;
            anti_diag_band_start_ptr[anti_diagonal] = band_start;
            anti_diag_band_end_ptr[anti_diagonal] = band_end;
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
                    backtrack_matrix + anti_diag_row_base
                    + static_cast<size_t>(band_vec_index - band_start_vec) * kVecBytes);
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
            if (ksw_apply_zdrop(&result, 1,
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
                if ((raw_flags & KSW_EZ_APPROX_DROP)
                    && ksw_apply_zdrop(&result, 1,
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
        const int cigar_reversed = (raw_flags & KSW_EZ_REV_CIGAR) != 0 ? 1 : 0;
        if (!result.zdropped && !(raw_flags & KSW_EZ_EXTZ_ONLY)) {
            ksw_backtrack(nullptr, 1, cigar_reversed, 0,
                reinterpret_cast<uint8_t*>(backtrack_matrix),
                anti_diag_band_start_ptr, anti_diag_band_end_ptr,
                band_vec_capacity * kVecBytes, target_len - 1, query_len - 1,
                &result.m_cigar, &result.n_cigar, &result.cigar);
        } else if (!result.zdropped && (raw_flags & KSW_EZ_EXTZ_ONLY)
                   && result.mqe + params.end_bonus > static_cast<int>(result.max))
        {
            result.reach_end = 1;
            ksw_backtrack(nullptr, 1, cigar_reversed, 0,
                reinterpret_cast<uint8_t*>(backtrack_matrix),
                anti_diag_band_start_ptr, anti_diag_band_end_ptr,
                band_vec_capacity * kVecBytes, result.mqe_t, query_len - 1,
                &result.m_cigar, &result.n_cigar, &result.cigar);
        } else if (result.max_t >= 0 && result.max_q >= 0) {
            ksw_backtrack(nullptr, 1, cigar_reversed, 0,
                reinterpret_cast<uint8_t*>(backtrack_matrix),
                anti_diag_band_start_ptr, anti_diag_band_end_ptr,
                band_vec_capacity * kVecBytes, result.max_t, result.max_q,
                &result.m_cigar, &result.n_cigar, &result.cigar);
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
 * @return Populated `ksw_extz_t` result.
 */
template<int AlphabetDim>
[[nodiscard]] ksw_extz_t hwy_align_d2(
    KswSequenceView query,
    KswSequenceView target,
    const KswScoringMatrix<AlphabetDim>& scoring_matrix,
    KswD2Params params)
{
    constexpr int kVecBytes = kKswVecBytes;

    KswByteTag  byte_tag;
    KswScoreTag score_tag;

    ksw_extz_t result = make_empty_extz();
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
    const int    raw_flags       = params.flags;
    const bool   compute_cigar   = (raw_flags & KSW_EZ_SCORE_ONLY) == 0;
    const bool   use_approx_max  = (raw_flags & KSW_EZ_APPROX_MAX) != 0;

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
            static_cast<size_t>(arena.target_vec_count()) * kVecBytes, KSW_NEG_INF);
        exact_score_row = exact_score_row_storage.data();
    }

    // --- Backtrack storage ----------------------------------------------
    KswBacktrackStorage backtrack;
    int8_t* backtrack_matrix = nullptr;
    int*    anti_diag_band_start_ptr = nullptr;
    int*    anti_diag_band_end_ptr   = nullptr;
    if (compute_cigar) {
        backtrack = make_backtrack_storage(query_len, target_len,
                                           band_vec_capacity, kVecBytes);
        backtrack_matrix         = backtrack.matrix_ptr();
        anti_diag_band_start_ptr = backtrack.band_start_ptr();
        anti_diag_band_end_ptr   = backtrack.band_end_ptr();
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
            result.zdropped = 1;
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
        if (!(raw_flags & KSW_EZ_GENERIC_SC)) {
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
        } else if (!(raw_flags & KSW_EZ_RIGHT)) {
            // --- CIGAR, gap left-alignment ----------------------
            const size_t anti_diag_row_base =
                static_cast<size_t>(anti_diagonal) * band_vec_capacity * kVecBytes;
            anti_diag_band_start_ptr[anti_diagonal] = band_start;
            anti_diag_band_end_ptr[anti_diagonal] = band_end;
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
                    backtrack_matrix + anti_diag_row_base
                    + static_cast<size_t>(band_vec_index - band_start_vec) * kVecBytes);
            }
        } else {
            // --- CIGAR, gap right-alignment ---------------------
            const size_t anti_diag_row_base =
                static_cast<size_t>(anti_diagonal) * band_vec_capacity * kVecBytes;
            anti_diag_band_start_ptr[anti_diagonal] = band_start;
            anti_diag_band_end_ptr[anti_diagonal] = band_end;
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
                    backtrack_matrix + anti_diag_row_base
                    + static_cast<size_t>(band_vec_index - band_start_vec) * kVecBytes);
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
            if (ksw_apply_zdrop(&result, 1,
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
            if ((raw_flags & KSW_EZ_APPROX_DROP)
                && ksw_apply_zdrop(&result, 1,
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
        const int cigar_reversed = (raw_flags & KSW_EZ_REV_CIGAR) != 0 ? 1 : 0;
        if (!result.zdropped && !(raw_flags & KSW_EZ_EXTZ_ONLY)) {
            ksw_backtrack(nullptr, 1, cigar_reversed, 0,
                reinterpret_cast<uint8_t*>(backtrack_matrix),
                anti_diag_band_start_ptr, anti_diag_band_end_ptr,
                band_vec_capacity * kVecBytes, target_len - 1, query_len - 1,
                &result.m_cigar, &result.n_cigar, &result.cigar);
        } else if (!result.zdropped && (raw_flags & KSW_EZ_EXTZ_ONLY)
                   && result.mqe + params.end_bonus > static_cast<int>(result.max))
        {
            result.reach_end = 1;
            ksw_backtrack(nullptr, 1, cigar_reversed, 0,
                reinterpret_cast<uint8_t*>(backtrack_matrix),
                anti_diag_band_start_ptr, anti_diag_band_end_ptr,
                band_vec_capacity * kVecBytes, result.mqe_t, query_len - 1,
                &result.m_cigar, &result.n_cigar, &result.cigar);
        } else if (result.max_t >= 0 && result.max_q >= 0) {
            ksw_backtrack(nullptr, 1, cigar_reversed, 0,
                reinterpret_cast<uint8_t*>(backtrack_matrix),
                anti_diag_band_start_ptr, anti_diag_band_end_ptr,
                band_vec_capacity * kVecBytes, result.max_t, result.max_q,
                &result.m_cigar, &result.n_cigar, &result.cigar);
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
    [[nodiscard]] static ksw_extz_t z2_align(
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
    [[nodiscard]] static ksw_extz_t d2_align(
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
    [[nodiscard]] static ksw_extz_t z2_align(
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
    [[nodiscard]] static ksw_extz_t d2_align(
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
