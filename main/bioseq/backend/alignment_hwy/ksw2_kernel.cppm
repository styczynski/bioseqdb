/**
 * @file ksw2_kernel.cppm
 * @brief Highway SIMD implementation of KSW2 extension alignment.
 *
 * Portable SIMD implementations of the KSW2 extension alignment (Suzuki-
 * Kasahara diagonal formulation). Uses Google Highway so the code compiles
 * and runs on x86 (SSE2–AVX-512), ARM NEON/SVE, WASM, and any future target
 * Highway supports.
 *
 * Single- and dual-affine gaps share one recurrence implementation and retain
 * the current KSW2 banding, traceback, Z-drop, and score semantics.
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
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

// The encode path uses the lazy wrapper's slot-ring accessor so target workers
// share the canonical bounded sequence materialization path.
using bioseq::storage::encode_region_via_ptr_at;

// =============================================================================
// ksw_aligner_mode — selects single- or dual-affine-gap formulation.
// =============================================================================

/** Affine-gap recurrence selected for one KSW2 invocation. */
enum class ksw_aligner_mode {
    single_affine,  ///< cost(k) = q + k*e
    dual_affine,    ///< cost(k) = min(q + k*e, q2 + k*e2)
};

// =============================================================================
// SIMD helpers — templated by tag p_simd_tag so no global type aliases are needed.
// =============================================================================

/** Return the per-lane maximum under unsigned-byte ordering. */
template <class p_simd_tag, class p_vector_type = hn::Vec<p_simd_tag>>
[[maybe_unused]] [[always_inline]] inline p_vector_type maximum_unsigned_bytes(
    const p_simd_tag& simd_tag,
    p_vector_type left,
    p_vector_type right) noexcept {
    hn::RebindToUnsigned<p_simd_tag> unsigned_tag;
    return hn::BitCast(simd_tag, hn::Max(hn::BitCast(unsigned_tag, left), hn::BitCast(unsigned_tag, right)));
}

/** Return the per-lane minimum under unsigned-byte ordering. */
template <class p_simd_tag, class p_vector_type = hn::Vec<p_simd_tag>>
[[maybe_unused]] [[always_inline]] inline p_vector_type minimum_unsigned_bytes(
    const p_simd_tag& simd_tag,
    p_vector_type left,
    p_vector_type right) noexcept {
    hn::RebindToUnsigned<p_simd_tag> unsigned_tag;
    return hn::BitCast(simd_tag, hn::Min(hn::BitCast(unsigned_tag, left), hn::BitCast(unsigned_tag, right)));
}

/** Return a zero vector with its first lane set to a scalar value. */
template <class p_simd_tag, class p_vector_type = hn::Vec<p_simd_tag>>
[[always_inline]] inline p_vector_type set_lane_zero(
    const p_simd_tag& simd_tag,
    hn::TFromD<p_simd_tag> value) noexcept {
    return hn::IfThenElse(
        hn::FirstN(simd_tag, 1u), hn::Set(simd_tag, value), hn::Zero(simd_tag));
}

/** Shift a vector left by one lane while carrying across vector boundaries. */
template <class p_simd_tag, class p_vector_type = hn::Vec<p_simd_tag>>
[[always_inline]] inline p_vector_type shift_left_insert(
    const p_simd_tag& simd_tag,
    p_vector_type vector,
    p_vector_type& inserted_lane) noexcept {
    constexpr size_t k_element_bytes = sizeof(hn::TFromD<p_simd_tag>);
    constexpr size_t k_maximum_vector_bytes =
        hn::MaxLanes(p_simd_tag{}) * sizeof(hn::TFromD<p_simd_tag>);
    if constexpr (k_maximum_vector_bytes <= 16u) {
        auto carry_out =
            hn::ShiftRightBytes<k_maximum_vector_bytes - k_element_bytes>(
                simd_tag,
                vector);
        auto shifted = hn::Or(
            hn::ShiftLeftBytes<k_element_bytes>(simd_tag, vector),
            inserted_lane);
        inserted_lane = carry_out;
        return shifted;
    } else {
        auto shifted = hn::SlideUpLanes(simd_tag, vector, 1u);
        shifted = hn::IfThenElse(hn::FirstN(simd_tag, 1u), inserted_lane, shifted);
        // Lanes(simd_tag), rather than its constexpr upper bound, is required for
        // scalable SVE/RVV vectors. SlideDown leaves the vacated last lane in
        // lane zero and zeros the remaining lanes, matching ShiftRightBytes.
        inserted_lane = hn::SlideDownLanes(
            simd_tag,
            vector,
            hn::Lanes(simd_tag) - 1u);
        return shifted;
    }
}

// =============================================================================
// Flags + parameter bundles
// =============================================================================

// =============================================================================
// Z-drop heuristic
// =============================================================================

/** Update maximum-score coordinates and report whether Z-drop terminated. */
[[nodiscard]] inline bool apply_z_drop_heuristic(
    ksw_alignment_result& result,
    bool use_rotated_coordinates,
    int32_t current_score,
    int first_coordinate,
    int second_coordinate,
    int z_drop_threshold,
    int8_t gap_extension_penalty) noexcept
{
    int anti_diagonal;
    int target_position;
    if (use_rotated_coordinates) {
        anti_diagonal   = first_coordinate;
        target_position = second_coordinate;
    } else {
        anti_diagonal   = first_coordinate + second_coordinate;
        target_position = first_coordinate;
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
        if (z_drop_threshold >= 0
            && result.max - current_score
               > z_drop_threshold + cd * gap_extension_penalty)
        {
            result.is_z_dropped = true;
            return true;
        }
    }
    return false;
}

// =============================================================================
// ksw_dp_state — named-field DP arena.
// =============================================================================

/** Arena-backed dynamic-programming state for the single-affine recurrence. */
class ksw_dp_state {
public:
    int8_t*  diff_up_base       = nullptr;  ///< Vertical score-difference rows.
    int8_t*  diff_left_base     = nullptr;  ///< Horizontal score-difference rows.
    int8_t*  gap_e_offset_base  = nullptr;  ///< Horizontal gap offsets.
    int8_t*  gap_f_offset_base  = nullptr;  ///< Vertical gap offsets.
    int8_t*  diag_score_base    = nullptr;  ///< Generic-score diagonal rows.
    uint8_t* target_bytes       = nullptr;  ///< Encoded target ranks.
    uint8_t* query_reversed     = nullptr;  ///< Encoded query ranks in DP order.

    /** Construct an empty state handle. */
    ksw_dp_state() = default;

    /** Allocate single-affine state for the requested sequence extents. */
    ksw_dp_state(size_t target_len, size_t query_len, size_t vector_bytes)
        : ksw_dp_state(target_len, query_len, vector_bytes, 5, /*zero_state_arrays=*/true) {}

    /** Return the number of vector slots in one target row. */
    [[nodiscard]] int target_vector_count() const noexcept { return target_vec_count_; }
    /** Return the number of vector slots needed for the query encoding. */
    [[nodiscard]] int query_vector_count()  const noexcept { return query_vec_count_; }
    /** Return the byte size of one target-oriented state array. */
    [[nodiscard]] size_t array_byte_count() const noexcept {
        return static_cast<size_t>(target_vec_count_) * vec_bytes_;
    }

    /** Fill the first state arrays with one byte value. */
    void fill_leading_arrays(int n, uint8_t value) noexcept {
        std::memset(diff_up_base, value, static_cast<size_t>(n) * array_byte_count());
    }

    /** Fill one state array with one byte value. */
    void fill_array(int8_t* base, uint8_t value) noexcept {
        std::memset(base, value, array_byte_count());
    }

protected:
    // Storage comes from the per-alignment arena. Only DP state that can be
    // observed before its first write is zeroed; the encoded query and target
    // byte regions are overwritten immediately by the encoding step.
    //
    // The `zero_state_arrays` constructor switch lets the dual-affine
    // subclass opt out of the broad memset(0): ksw_dual_affine_dp_state immediately
    // overwrites arrays 0..3 with `-q-e` and arrays 4..5 with
    // `-q2-e2`, so zeroing them first is wasted bandwidth — for very-long
    // alignments this is megabyte-scale memset traffic per call. The
    // diag_score array (the last DP-state slot) is still zero-filled
    // because the generic-score code path may read it before writing,
    // and it's only one stride wide.
    ksw_dp_state(size_t target_len, size_t query_len, size_t vector_bytes,
               int state_count, bool zero_state_arrays)
        : target_vec_count_(static_cast<int>((target_len + vector_bytes - 1) / vector_bytes))
        , query_vec_count_(static_cast<int>((query_len + vector_bytes - 1) / vector_bytes))
        , vec_bytes_(vector_bytes)
    {
        const size_t stride = static_cast<size_t>(target_vec_count_) * vector_bytes;
        const size_t total  = (static_cast<size_t>(state_count + 1) * target_vec_count_
                              + static_cast<size_t>(query_vec_count_) + 1) * vector_bytes;
        storage_ = context::current().alloc_align_array<int8_t, 64>(total);
        const size_t dp_bytes = static_cast<size_t>(state_count) * stride;
        if (zero_state_arrays) {
            std::memset(storage_.data(), 0, dp_bytes);
        } else {
            // Only zero the diag_score slot (the last state slab). Arrays
            // 0..(state_count-2) are filled by the caller before first read.
            std::memset(storage_.data() + static_cast<size_t>(state_count - 1) * stride,
                        0, stride);
        }
        // Wipe the single extra byte read just past the DP-state arrays through
        // diag_score_base so the boundary remains deterministic.
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

    [[nodiscard]] int8_t* array_pointer(int index) noexcept {
        return storage_.data()
            + static_cast<size_t>(index) * target_vec_count_ * vec_bytes_;
    }

    int target_vec_count_ = 0;
    int query_vec_count_  = 0;
    size_t vec_bytes_     = 0;
    std::span<int8_t> storage_;
};

/** Extended DP state carrying the second affine-gap recurrence. */
class ksw_dual_affine_dp_state : public ksw_dp_state {
public:
    int8_t* gap_e2_offset_base = nullptr;  ///< Long horizontal-gap offsets.
    int8_t* gap_f2_offset_base = nullptr;  ///< Long vertical-gap offsets.

    /** Construct an empty dual-affine state handle. */
    ksw_dual_affine_dp_state() = default;

    /** Allocate dual-affine state for the requested sequence extents. */
    ksw_dual_affine_dp_state(size_t target_len, size_t query_len, size_t vector_bytes)
        // Skip the broad zero-fill: align_ksw2_core fills arrays 0..5
        // explicitly via fill_leading_arrays(4, -q-e) +
        // fill_array(x2|y2, -q2-e2) right after construction. The
        // diag_score slot (array 6) still gets a one-stride zero-fill
        // inside the parent constructor.
        : ksw_dp_state(target_len, query_len, vector_bytes, 7,
                     /*zero_state_arrays=*/false)
    {
        gap_e2_offset_base = array_pointer(4);
        gap_f2_offset_base = array_pointer(5);
    }
};

/** Select the arena-backed DP state required by an affine-gap mode. */
template <ksw_aligner_mode p_mode>
using ksw_state_for = std::conditional_t<
    p_mode == ksw_aligner_mode::dual_affine,
    ksw_dual_affine_dp_state,
    ksw_dp_state>;

// =============================================================================
// ksw_backtrack_matrix
// =============================================================================

/** Arena-backed banded traceback matrix and per-row band bounds. */
class ksw_backtrack_matrix {
public:
    /** Construct an empty traceback matrix. */
    ksw_backtrack_matrix() noexcept = default;

    /** Allocate storage for the admitted anti-diagonals and band capacity. */
    void allocate(int anti_diagonal_count, int band_vector_capacity, size_t vector_bytes) {
        vec_bytes_ = vector_bytes;
        cols_in_vectors_ = band_vector_capacity;
        byte_cols_per_row_ = band_vector_capacity * static_cast<int>(vector_bytes);
        total_vector_slots_ =
            static_cast<size_t>(anti_diagonal_count) * cols_in_vectors_ + 1;
        const size_t total_bytes = total_vector_slots_ * vector_bytes;
        auto& ctx = context::current();
        storage_ = ctx.alloc_align_array<int8_t, 64>(total_bytes);
        band_range_ = ctx.alloc_align_array<int32_t>(
            2 * static_cast<size_t>(anti_diagonal_count));
    }

    /** Return whether traceback storage has been allocated. */
    [[nodiscard]] bool is_allocated() const noexcept { return !storage_.empty(); }

    /** Return the byte storage for one vector slot in an anti-diagonal. */
    [[nodiscard]] int8_t* vector_slot_bytes(int anti_diagonal, int local_band_vec) noexcept {
        const size_t slot =
            static_cast<size_t>(anti_diagonal) * cols_in_vectors_
            + static_cast<size_t>(local_band_vec);
        return storage_.data() + slot * vec_bytes_;
    }

    /** Return the immutable traceback byte stream. */
    [[nodiscard]] const uint8_t* bytes() const noexcept {
        return reinterpret_cast<const uint8_t*>(storage_.data());
    }
    /** Return the byte stride between consecutive anti-diagonal rows. */
    [[nodiscard]] int byte_columns_per_row() const noexcept { return byte_cols_per_row_; }
    /** Return mutable start coordinates for every anti-diagonal band. */
    [[nodiscard]] std::span<int32_t> band_starts() noexcept {
        return band_range_.subspan(0, band_range_.size() / 2);
    }
    /** Return mutable end coordinates for every anti-diagonal band. */
    [[nodiscard]] std::span<int32_t> band_ends() noexcept {
        return band_range_.subspan(band_range_.size() / 2);
    }
    /** Return immutable start coordinates for every anti-diagonal band. */
    [[nodiscard]] std::span<const int32_t> band_starts() const noexcept {
        return std::span<const int32_t>(band_range_.data(), band_range_.size() / 2);
    }
    /** Return immutable end coordinates for every anti-diagonal band. */
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

/** Reconstruct a packed CIGAR from the banded traceback matrix. */
inline void ksw_backtrack_cigar(
    const ksw_backtrack_matrix& matrix,
    bool use_rotated_coordinates,
    bool output_reversed,
    int minimum_intron_length,
    int starting_row,
    int starting_column,
    std::vector<uint32_t>& output_cigar)
{
    output_cigar.clear();
    // The traceback walks (starting_row + starting_column + 1) cells. Each cell
    // emits one packed-cigar push, but `push_packed_cigar` merges with
    // the previous op when the opcode matches — so for a typical
    // low-error long-read alignment the final CIGAR is ~5 ops per 100 bp
    // (~5 % of cells emit a fresh op). Reserve `(cells + 7) / 8` to
    // sidestep the geometric-realloc chain during the backward build:
    // every doubling realloc memcpys the previous contents, which on a
    // 1 Mbp alignment dominates the traceback wall-clock. Slight over-
    // estimation is fine — the vector is short-lived (lives on the
    // result struct, returned by value) and the alternative is the
    // repeated geometric reallocation for long reads.
    const size_t cell_estimate = static_cast<size_t>(starting_row + starting_column + 1);
    output_cigar.reserve(std::max<size_t>(64, (cell_estimate + 7) / 8));

    const uint8_t* bt = matrix.bytes();
    const std::span<const int32_t> off = matrix.band_starts();
    const std::span<const int32_t> off_end = matrix.band_ends();
    const int n_col = matrix.byte_columns_per_row();

    int i = starting_row, k = starting_column, state = 0;
    while (i >= 0 && k >= 0) {
        int force = -1;
        uint32_t bv = 0;
        if (use_rotated_coordinates) {
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

        if (state == 0)      { push_packed_cigar(output_cigar, cigar_match_opcode, 1); --i; --k; }
        else if (state == 1 || (state == 3 && minimum_intron_length <= 0))
                             { push_packed_cigar(output_cigar, cigar_deletion_opcode, 1); --i; }
        else if (state == 3 && minimum_intron_length > 0)
                             { push_packed_cigar(output_cigar, cigar_reference_skip_opcode, 1); --i; }
        else                 { push_packed_cigar(output_cigar, cigar_insertion_opcode, 1); --k; }
    }

    if (i >= 0) {
        const uint32_t op = (minimum_intron_length > 0 && i >= minimum_intron_length)
            ? cigar_reference_skip_opcode : cigar_deletion_opcode;
        push_packed_cigar(output_cigar, op, static_cast<uint32_t>(i + 1));
    }
    if (k >= 0) push_packed_cigar(output_cigar, cigar_insertion_opcode, static_cast<uint32_t>(k + 1));
    if (!output_reversed) std::reverse(output_cigar.begin(), output_cigar.end());
}

// =============================================================================
// ksw_dp_cell — values loaded for one DP update.
// =============================================================================

/** Vector state returned by one KSW2 DP load step. */
template <class p_simd_tag>
struct ksw_dp_cell {
    hn::Vec<p_simd_tag> score_max_cell;  ///< Selected score for every lane.
    hn::Vec<p_simd_tag> diff_left_sh;    ///< Shifted horizontal differences.
    hn::Vec<p_simd_tag> diff_up_cur;     ///< Current vertical differences.
    hn::Vec<p_simd_tag> diag_chain;      ///< Short horizontal-gap candidates.
    hn::Vec<p_simd_tag> vert_chain;      ///< Short vertical-gap candidates.
    hn::Vec<p_simd_tag> diag_chain2;     ///< Long horizontal-gap candidates.
    hn::Vec<p_simd_tag> vert_chain2;     ///< Long vertical-gap candidates.

    // A project-declared body gives the generator a callable on which to put
    // HWY_ATTR. Default-initialization still leaves every vector uninitialized.
    /** Leave lane vectors uninitialized for immediate kernel population. */
    [[gnu::always_inline]] ksw_dp_cell() noexcept {}
};

// =============================================================================
// load_ksw_dp_cell — load one SIMD DP cell.
//
// When p_is_fused_score == true the cell score is computed inline from sequence
// data while preserving fused diagonal-score storage.
// When false the score is loaded from diag_score_base (generic_score path).
// =============================================================================

/** Load recurrence state and construct one vector of candidate scores. */
template <
    ksw_aligner_mode p_mode,
    bool p_is_fused_score,
    class p_simd_tag,
    class p_state_type>
[[always_inline]] inline ksw_dp_cell<p_simd_tag> load_ksw_dp_cell(
    p_simd_tag simd_tag, int off,
    p_state_type& HWY_RESTRICT state,
    const uint8_t* HWY_RESTRICT query_reversed_aligned,
    hn::Vec<p_simd_tag>& bx_vec, hn::Vec<p_simd_tag>& bv_vec,
    [[maybe_unused]] hn::Vec<p_simd_tag>& bx2_vec,
    hn::Vec<p_simd_tag> double_gap_cost_vec,
    hn::Vec<p_simd_tag> match_score_vec,
    hn::Vec<p_simd_tag> mismatch_score_vec,
    hn::Vec<p_simd_tag> wildcard_score_vec,
    hn::Vec<p_simd_tag> wildcard_rank_vec) noexcept
{
    constexpr bool k_is_dual_affine =
        p_mode == ksw_aligner_mode::dual_affine;
    using partial_score_type = hn::TFromD<p_simd_tag>;
    ksw_dp_cell<p_simd_tag> c;

    // --- Cell score ---
    if constexpr (p_is_fused_score) {
        auto tl = hn::LoadU(simd_tag,
            reinterpret_cast<const partial_score_type*>(&state.target_bytes[off]));
        auto ql = hn::LoadU(simd_tag,
            reinterpret_cast<const partial_score_type*>(&query_reversed_aligned[off]));
        auto wm = hn::Or(
            hn::VecFromMask(simd_tag, hn::Eq(tl, wildcard_rank_vec)),
            hn::VecFromMask(simd_tag, hn::Eq(ql, wildcard_rank_vec)));
        auto eq = hn::VecFromMask(simd_tag, hn::Eq(tl, ql));
        c.score_max_cell = hn::IfThenElse(
            hn::MaskFromVec(eq), match_score_vec, mismatch_score_vec);
        c.score_max_cell = hn::IfThenElse(
            hn::MaskFromVec(wm), wildcard_score_vec, c.score_max_cell);
    } else {
        c.score_max_cell = hn::Load(simd_tag, state.diag_score_base + off);
    }

    if constexpr (!k_is_dual_affine) {
        c.score_max_cell = hn::Add(c.score_max_cell, double_gap_cost_vec);
    }

    // --- Gap / diff state ---
    auto gap_e_sh = hn::Load(simd_tag, state.gap_e_offset_base + off);
    gap_e_sh = shift_left_insert(simd_tag, gap_e_sh, bx_vec);
    c.diff_left_sh = hn::Load(simd_tag, state.diff_left_base + off);
    c.diff_left_sh = shift_left_insert(simd_tag, c.diff_left_sh, bv_vec);
    c.diag_chain = hn::Add(gap_e_sh, c.diff_left_sh);
    c.diff_up_cur = hn::Load(simd_tag, state.diff_up_base + off);
    c.vert_chain = hn::Add(
        hn::Load(simd_tag, state.gap_f_offset_base + off), c.diff_up_cur);

    c.diag_chain2 = hn::Zero(simd_tag);
    c.vert_chain2 = hn::Zero(simd_tag);
    if constexpr (k_is_dual_affine) {
        auto ge2_sh = hn::Load(simd_tag, state.gap_e2_offset_base + off);
        ge2_sh = shift_left_insert(simd_tag, ge2_sh, bx2_vec);
        c.diag_chain2 = hn::Add(ge2_sh, c.diff_left_sh);
        c.vert_chain2 = hn::Add(
            hn::Load(simd_tag, state.gap_f2_offset_base + off), c.diff_up_cur);
    }

    return c;
}

// =============================================================================
// reduce_ksw_dp_maximum — reduce per-lane maxima into the result state.
// =============================================================================

/** Reduce one vector of recurrence candidates to the selected cell scores. */
template <ksw_aligner_mode p_mode, class p_simd_tag>
[[always_inline]] inline void reduce_ksw_dp_maximum(
    p_simd_tag simd_tag, ksw_dp_cell<p_simd_tag>& c,
    hn::Vec<p_simd_tag> max_plausible_score_vec,
    hn::Vec<p_simd_tag> match_score_vec) noexcept
{
    constexpr bool k_is_dual_affine =
        p_mode == ksw_aligner_mode::dual_affine;

    c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain);
    if constexpr (!k_is_dual_affine) {
        c.score_max_cell = maximum_unsigned_bytes(simd_tag, c.score_max_cell, c.vert_chain);
        c.score_max_cell = minimum_unsigned_bytes(simd_tag, c.score_max_cell, max_plausible_score_vec);
    } else {
        c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain);
        c.score_max_cell = hn::Max(c.score_max_cell, c.diag_chain2);
        c.score_max_cell = hn::Max(c.score_max_cell, c.vert_chain2);
        c.score_max_cell = hn::Min(c.score_max_cell, match_score_vec);
    }
}

// =============================================================================
// store_ksw_dp_cell — store one SIMD DP update.
// =============================================================================

/** Store one vector of selected scores and updated gap offsets. */
template <
    ksw_aligner_mode p_mode,
    class p_simd_tag,
    class p_state_type>
[[always_inline]] inline void store_ksw_dp_cell(
    p_simd_tag simd_tag, int off,
    ksw_dp_cell<p_simd_tag>& c,
    p_state_type& HWY_RESTRICT state,
    hn::Vec<p_simd_tag> gap_open_vec,
    hn::Vec<p_simd_tag> gap_open_short_vec,
    hn::Vec<p_simd_tag> gap_open_long_vec,
    hn::Vec<p_simd_tag> short_gap_cost_vec,
    hn::Vec<p_simd_tag> long_gap_cost_vec,
    hn::Vec<p_simd_tag> zero_vec) noexcept
{
    constexpr bool k_is_dual_affine =
        p_mode == ksw_aligner_mode::dual_affine;

    hn::Store(hn::Sub(c.score_max_cell, c.diff_left_sh), simd_tag,
              state.diff_up_base + off);
    hn::Store(hn::Sub(c.score_max_cell, c.diff_up_cur), simd_tag,
              state.diff_left_base + off);

    if constexpr (!k_is_dual_affine) {
        auto smq = hn::Sub(c.score_max_cell, gap_open_vec);
        c.diag_chain = hn::Sub(c.diag_chain, smq);
        c.vert_chain = hn::Sub(c.vert_chain, smq);
        hn::Store(hn::Max(c.diag_chain, zero_vec), simd_tag,
                  state.gap_e_offset_base + off);
        hn::Store(hn::Max(c.vert_chain, zero_vec), simd_tag,
                  state.gap_f_offset_base + off);
    } else {
        auto sms = hn::Sub(c.score_max_cell, gap_open_short_vec);
        c.diag_chain = hn::Sub(c.diag_chain, sms);
        c.vert_chain = hn::Sub(c.vert_chain, sms);
        auto sml = hn::Sub(c.score_max_cell, gap_open_long_vec);
        c.diag_chain2 = hn::Sub(c.diag_chain2, sml);
        c.vert_chain2 = hn::Sub(c.vert_chain2, sml);
        hn::Store(hn::Sub(hn::Max(c.diag_chain, zero_vec), short_gap_cost_vec),
                  simd_tag, state.gap_e_offset_base + off);
        hn::Store(hn::Sub(hn::Max(c.vert_chain, zero_vec), short_gap_cost_vec),
                  simd_tag, state.gap_f_offset_base + off);
        hn::Store(hn::Sub(hn::Max(c.diag_chain2, zero_vec), long_gap_cost_vec),
                  simd_tag, state.gap_e2_offset_base + off);
        hn::Store(hn::Sub(hn::Max(c.vert_chain2, zero_vec), long_gap_cost_vec),
                  simd_tag, state.gap_f2_offset_base + off);
    }
}

// =============================================================================
// align_ksw2_core — unified single/dual-affine banded extension alignment
// =============================================================================

template<
    ksw_aligner_mode p_mode,
    bool p_is_reversed,
    sequence_alphabet p_alphabet,
    size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u,
    class p_partial_score_type = int8_t,
    class p_full_score_type = int32_t>
/** Execute the banded KSW2 recurrence for one sequence pair. */
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
    int band_width_param,
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
    const int band_width = band_width_param < 0
        ? std::max(tlen, qlen) : band_width_param;
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
        encode_region_via_ptr_at<p_is_reversed, false>(
            query_seq, query_dst, query_start, query_end);
        // Target is reversed; no rc on target, so the second template arg is
        // the only orientation flip.
        encode_region_via_ptr_at<false, true>(
            target_seq, target_dst, target_start, target_end);
    } else {
        // qr = reverse(encoded query). For a reversed strand the
        // existing <true, false> path produced rc(query); we want rc then
        // reverse-again, which is just complement-only. Captured as
        // <true, true> (see overload comment).
        encode_region_via_ptr_at<p_is_reversed, true>(
            query_seq, query_dst, query_start, query_end);
        encode_region_via_ptr_at<false, false>(
            target_seq, target_dst, target_start, target_end);
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

                auto max_sv = hn::Set(full_score_tag, anti_diag_max_score);
                auto max_pv = hn::Set(full_score_tag, static_cast<p_full_score_type>(anti_diag_max_target_pos));
                [[maybe_unused]] auto goe_vec = hn::Set(full_score_tag,
                    static_cast<p_full_score_type>(gap_open_plus_extend));

                const int block_end = band_start_raw
                    + (band_end_raw - band_start_raw) / static_cast<int>(score_lane_count)
                      * static_cast<int>(score_lane_count);
                int tp = band_start_raw;
                for (; tp < block_end; tp += static_cast<int>(score_lane_count)) {
                    auto es = hn::LoadU(full_score_tag, &exact_score_row[tp]);
                    HWY_ALIGN p_full_score_type dl_buf[k_maximum_score_lanes] = {};
                    for (size_t li = 0; li < score_lane_count; ++li) {
                        if constexpr (!k_is_dual_affine)
                            dl_buf[li] = static_cast<p_full_score_type>(
                                reinterpret_cast<const uint8_t*>(state.diff_left_base)[tp + li]);
                        else
                            dl_buf[li] = static_cast<p_full_score_type>(
                                state.diff_left_base[tp + li]);
                    }
                    auto dlv = hn::Load(full_score_tag, dl_buf);
                    es = hn::Add(es, dlv);
                    if constexpr (!k_is_dual_affine) {
                        es = hn::Sub(es, goe_vec);
                    }
                    hn::StoreU(es, full_score_tag, &exact_score_row[tp]);

                    auto tpv = hn::Set(full_score_tag, static_cast<p_full_score_type>(tp));
                    auto gm = hn::Gt(es, max_sv);
                    max_sv = hn::IfThenElse(gm, es, max_sv);
                    max_pv = hn::IfThenElse(gm, tpv, max_pv);
                }

                HWY_ALIGN p_full_score_type sb[k_maximum_score_lanes];
                HWY_ALIGN p_full_score_type pb[k_maximum_score_lanes];
                hn::Store(max_sv, full_score_tag, sb);
                hn::Store(max_pv, full_score_tag, pb);
                for (size_t li = 0; li < score_lane_count; ++li) {
                    if (anti_diag_max_score < sb[li]) {
                        anti_diag_max_score = sb[li];
                        anti_diag_max_target_pos = static_cast<int>(pb[li])
                            + static_cast<int>(li);
                    }
                }
                for (; tp < band_end_raw; ++tp) {
                    if constexpr (!k_is_dual_affine) {
                        exact_score_row[tp] += static_cast<p_full_score_type>(
                            reinterpret_cast<const uint8_t*>(state.diff_left_base)[tp])
                            - gap_open_plus_extend;
                    } else {
                        exact_score_row[tp] += static_cast<p_full_score_type>(
                            state.diff_left_base[tp]);
                    }
                    if (exact_score_row[tp] > anti_diag_max_score) {
                        anti_diag_max_score = exact_score_row[tp];
                        anti_diag_max_target_pos = tp;
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
    template<
        bool p_is_reversed,
        sequence_alphabet p_alphabet>
    /** Align with the single-affine recurrence. */
    [[nodiscard]] static ksw_alignment_result align_single_affine(
        bioseq::storage::lazy_sequence_view<p_alphabet> query,
        bioseq::storage::lazy_sequence_view<p_alphabet> target,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const ksw_single_affine_parameters& params)
    {
        return align_ksw2_core<ksw_aligner_mode::single_affine, p_is_reversed>(
            query, target, query_start, query_end, target_start, target_end,
            scoring_matrix,
            params.gap_open, params.gap_extend,
            params.gap_open, params.gap_extend,
            params.band_width, params.z_drop, params.end_bonus, params.flags);
    }

    template<
        bool p_is_reversed,
        sequence_alphabet p_alphabet>
    /** Align with the dual-affine recurrence. */
    [[nodiscard]] static ksw_alignment_result align_dual_affine(
        bioseq::storage::lazy_sequence_view<p_alphabet> query,
        bioseq::storage::lazy_sequence_view<p_alphabet> target,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        ksw_dual_affine_parameters params)
    {
        if (params.gap_open_long + params.gap_extend_long
            < params.gap_open_short + params.gap_extend_short)
        {
            std::swap(params.gap_open_short, params.gap_open_long);
            std::swap(params.gap_extend_short, params.gap_extend_long);
        }
        return align_ksw2_core<ksw_aligner_mode::dual_affine, p_is_reversed>(
            query, target, query_start, query_end, target_start, target_end,
            scoring_matrix,
            params.gap_open_short, params.gap_extend_short,
            params.gap_open_long, params.gap_extend_long,
            params.band_width, params.z_drop, params.end_bonus, params.flags);
    }
};

/** Dispatch the single-affine callable for the requested orientation. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result align_ksw2_single_affine(
    bool is_reversed,
    bioseq::storage::lazy_sequence_view<p_alphabet> query,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const scoring_matrix<p_alphabet>& scoring_matrix,
    const ksw_single_affine_parameters& params)
{
    return is_reversed
        ? highway_ksw2_aligner::align_single_affine<true>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, params)
        : highway_ksw2_aligner::align_single_affine<false>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, params);
}

/** Dispatch the dual-affine callable for the requested orientation. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result align_ksw2_dual_affine(
    bool is_reversed,
    bioseq::storage::lazy_sequence_view<p_alphabet> query,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const scoring_matrix<p_alphabet>& scoring_matrix,
    ksw_dual_affine_parameters params)
{
    return is_reversed
        ? highway_ksw2_aligner::align_dual_affine<true>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, params)
        : highway_ksw2_aligner::align_dual_affine<false>(
            query, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, params);
}

}  // namespace target
}  // namespace bioseq::simd
