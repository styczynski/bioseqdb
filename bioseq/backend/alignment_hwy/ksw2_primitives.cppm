/**
 * @file ksw2_primitives.cppm
 * @brief Defines the SIMD state, cell, and traceback primitives used by KSW2.
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

export module bioseq.highway.sources.ksw2_primitives;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

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

}  // namespace target
}  // namespace bioseq::simd
