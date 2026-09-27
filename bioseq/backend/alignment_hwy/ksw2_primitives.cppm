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
        const int target_distance = target_position - result.max_t;
        const int query_distance = (anti_diagonal - target_position) - result.max_q;
        const int diagonal_distance = target_distance > query_distance ? target_distance - query_distance : query_distance - target_distance;
        if (z_drop_threshold >= 0
            && result.max - current_score
               > z_drop_threshold + diagonal_distance * gap_extension_penalty)
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
    ksw2_partial_score_type* diff_up_base = nullptr;  ///< Vertical score-difference rows.
    ksw2_partial_score_type* diff_left_base = nullptr;  ///< Horizontal score-difference rows.
    ksw2_partial_score_type* gap_e_offset_base = nullptr;  ///< Horizontal gap offsets.
    ksw2_partial_score_type* gap_f_offset_base = nullptr;  ///< Vertical gap offsets.
    ksw2_partial_score_type* diag_score_base = nullptr;  ///< Generic-score diagonal rows.
    uint8_t* target_bytes       = nullptr;  ///< Encoded target ranks.
    uint8_t* query_reversed     = nullptr;  ///< Encoded query ranks in DP order.

    /** Construct an empty state handle. */
    ksw_dp_state() = default;

    /** Allocate single-affine state for the requested sequence extents. */
    explicit ksw_dp_state(const ksw2_storage_plan& plan)
        : ksw_dp_state(plan, /*zero_state_arrays=*/true) {}

    /** Return the number of vector slots in one target row. */
    [[nodiscard]] int target_vector_count() const noexcept { return target_vec_count_; }
    /** Return the number of vector slots needed for the query encoding. */
    [[nodiscard]] int query_vector_count()  const noexcept { return query_vec_count_; }
    /** Return the byte size of one target-oriented state array. */
    [[nodiscard]] size_t array_byte_count() const noexcept {
        return static_cast<size_t>(target_vec_count_) * vec_bytes_;
    }

    /** Fill the first state arrays with one byte value. */
    void fill_leading_arrays(int array_count, uint8_t value) noexcept {
        std::memset(diff_up_base, value, static_cast<size_t>(array_count) * array_byte_count());
    }

    /** Fill one state array with one byte value. */
    void fill_array(ksw2_partial_score_type* base, uint8_t value) noexcept {
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
    ksw_dp_state(const ksw2_storage_plan& plan, bool zero_state_arrays)
        : target_vec_count_(static_cast<int>(plan.target_vector_count))
        , query_vec_count_(static_cast<int>(plan.query_vector_count))
        , vec_bytes_(plan.vector_bytes)
    {
        const size_t state_array_count = plan.state_array_count;
        const size_t state_array_byte_count = plan.state_array_bytes;
        storage_ = context::current().alloc_align_array<ksw2_partial_score_type, 64>(
            plan.state_bytes);
        const size_t initialized_state_bytes = static_cast<size_t>(state_array_count) * state_array_byte_count;
        if (zero_state_arrays) {
            std::memset(storage_.data(), 0, initialized_state_bytes);
        } else {
            // Only zero the diag_score slot (the last state slab). Arrays
            // 0..(state_array_count-2) are filled by the caller before first
            // read.
            std::memset(storage_.data() + static_cast<size_t>(state_array_count - 1) * state_array_byte_count,
                        0, state_array_byte_count);
        }
        // Wipe the single extra byte read just past the DP-state arrays through
        // diag_score_base so the boundary remains deterministic.
        if (storage_.size() > initialized_state_bytes) {
            storage_[initialized_state_bytes] = 0;
        }

        ksw2_partial_score_type* storage_base = storage_.data();
        diff_up_base      = storage_base;
        diff_left_base    = diff_up_base      + state_array_byte_count;
        gap_e_offset_base = diff_left_base    + state_array_byte_count;
        gap_f_offset_base = gap_e_offset_base + state_array_byte_count;
        diag_score_base   = storage_base + static_cast<size_t>(state_array_count - 1) * state_array_byte_count;
        target_bytes      = reinterpret_cast<uint8_t*>(
            storage_base + static_cast<size_t>(state_array_count) * state_array_byte_count);
        query_reversed    = reinterpret_cast<uint8_t*>(
            storage_base + static_cast<size_t>(state_array_count + 1) * state_array_byte_count);
    }

    [[nodiscard]] ksw2_partial_score_type* array_pointer(int index) noexcept {
        return storage_.data()
            + static_cast<size_t>(index) * target_vec_count_ * vec_bytes_;
    }

    int target_vec_count_ = 0;
    int query_vec_count_  = 0;
    size_t vec_bytes_     = 0;
    std::span<ksw2_partial_score_type> storage_;
};

/** Extended DP state carrying the second affine-gap recurrence. */
class ksw_dual_affine_dp_state : public ksw_dp_state {
public:
    ksw2_partial_score_type* gap_e2_offset_base = nullptr;  ///< Long horizontal-gap offsets.
    ksw2_partial_score_type* gap_f2_offset_base = nullptr;  ///< Long vertical-gap offsets.

    /** Construct an empty dual-affine state handle. */
    ksw_dual_affine_dp_state() = default;

    /** Allocate dual-affine state for the requested sequence extents. */
    explicit ksw_dual_affine_dp_state(const ksw2_storage_plan& plan)
        // Skip the broad zero-fill: align_ksw2_core fills arrays 0..5
        // explicitly via fill_leading_arrays(4, -q-e) +
        // fill_array(x2|y2, -q2-e2) right after construction. The
        // diag_score slot (array 6) still gets a one-stride zero-fill
        // inside the parent constructor.
        : ksw_dp_state(plan, /*zero_state_arrays=*/false)
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
    void allocate(const ksw2_storage_plan& plan) {
        vec_bytes_ = plan.vector_bytes;
        cols_in_vectors_ = static_cast<int>(plan.band_vector_capacity);
        byte_cols_per_row_ = static_cast<int>(plan.band_vector_capacity * plan.vector_bytes);
        total_vector_slots_ = plan.backtrack_vector_slots;
        auto& alignment_context = context::current();
        storage_ = alignment_context.alloc_align_array<int8_t, 64>(plan.backtrack_bytes);
        band_range_ = alignment_context.alloc_align_array<int32_t>(plan.band_bound_elements);
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
inline void ksw_backtrack_cigar(
    const ksw_backtrack_matrix& matrix,
    bool use_rotated_coordinates,
    bool output_reversed,
    int minimum_intron_length,
    int starting_row,
    int starting_column,
    alignment_result_vector<std::uint32_t>& output_cigar)
#else
inline void ksw_backtrack_cigar(
    const ksw_backtrack_matrix& matrix,
    bool use_rotated_coordinates,
    bool output_reversed,
    int minimum_intron_length,
    int starting_row,
    int starting_column,
    std::vector<uint32_t>& output_cigar)
#endif
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

    const uint8_t* traceback_bytes = matrix.bytes();
    const std::span<const int32_t> band_starts = matrix.band_starts();
    const std::span<const int32_t> band_ends = matrix.band_ends();
    const int byte_columns_per_row = matrix.byte_columns_per_row();

    int row_position = starting_row, column_position = starting_column, traceback_state = 0;
    while (row_position >= 0 && column_position >= 0) {
        int forced_state = -1;
        uint32_t traceback_value = 0;
        if (use_rotated_coordinates) {
            const int anti_diagonal = row_position + column_position;
            if (row_position < band_starts[static_cast<size_t>(anti_diagonal)]) forced_state = 2;
            if (row_position > band_ends[static_cast<size_t>(anti_diagonal)]) forced_state = 1;
            if (forced_state < 0)
                traceback_value = traceback_bytes[static_cast<size_t>(anti_diagonal) * byte_columns_per_row
                        + static_cast<size_t>(row_position - band_starts[static_cast<size_t>(anti_diagonal)])];
        } else {
            if (column_position < band_starts[static_cast<size_t>(row_position)]) forced_state = 2;
            if (column_position > band_ends[static_cast<size_t>(row_position)]) forced_state = 1;
            if (forced_state < 0)
                traceback_value = traceback_bytes[static_cast<size_t>(row_position) * byte_columns_per_row
                        + static_cast<size_t>(column_position - band_starts[static_cast<size_t>(row_position)])];
        }

        if (traceback_state == 0) traceback_state = static_cast<int>(traceback_value & 7U);
        else if (!((traceback_value >> (traceback_state + 2)) & 1U)) traceback_state = 0;
        if (traceback_state == 0) traceback_state = static_cast<int>(traceback_value & 7U);
        if (forced_state >= 0) traceback_state = forced_state;

        if (traceback_state == 0)      { push_packed_cigar(output_cigar, cigar_match_opcode, 1); --row_position; --column_position; }
        else if (traceback_state == 1 || (traceback_state == 3 && minimum_intron_length <= 0))
                             { push_packed_cigar(output_cigar, cigar_deletion_opcode, 1); --row_position; }
        else if (traceback_state == 3 && minimum_intron_length > 0)
                             { push_packed_cigar(output_cigar, cigar_reference_skip_opcode, 1); --row_position; }
        else                 { push_packed_cigar(output_cigar, cigar_insertion_opcode, 1); --column_position; }
    }

    if (row_position >= 0) {
        const uint32_t operation = (minimum_intron_length > 0 && row_position >= minimum_intron_length)
            ? cigar_reference_skip_opcode : cigar_deletion_opcode;
        push_packed_cigar(output_cigar, operation, static_cast<uint32_t>(row_position + 1));
    }
    if (column_position >= 0) push_packed_cigar(output_cigar, cigar_insertion_opcode, static_cast<uint32_t>(column_position + 1));
    if (!output_reversed) std::reverse(output_cigar.begin(), output_cigar.end());
}

// =============================================================================
// ksw_dp_cell — values loaded for one DP update.
// =============================================================================

/** Vector state returned by one KSW2 DP load step. */
template <class p_simd_tag>
struct ksw_dp_cell {
    hn::Vec<p_simd_tag> selected_scores;  ///< Selected score for every lane.
    hn::Vec<p_simd_tag> shifted_left_differences;    ///< Shifted horizontal differences.
    hn::Vec<p_simd_tag> current_up_differences;     ///< Current vertical differences.
    hn::Vec<p_simd_tag> short_horizontal_gap_candidates;      ///< Short horizontal-gap candidates.
    hn::Vec<p_simd_tag> short_vertical_gap_candidates;      ///< Short vertical-gap candidates.
    hn::Vec<p_simd_tag> long_horizontal_gap_candidates;     ///< Long horizontal-gap candidates.
    hn::Vec<p_simd_tag> long_vertical_gap_candidates;     ///< Long vertical-gap candidates.

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
    p_simd_tag simd_tag, int state_offset,
    p_state_type& HWY_RESTRICT state,
    const uint8_t* HWY_RESTRICT query_reversed_aligned,
    hn::Vec<p_simd_tag>& horizontal_gap_carry, hn::Vec<p_simd_tag>& horizontal_difference_carry,
    [[maybe_unused]] hn::Vec<p_simd_tag>& long_horizontal_gap_carry,
    hn::Vec<p_simd_tag> double_gap_costs,
    hn::Vec<p_simd_tag> match_scores,
    hn::Vec<p_simd_tag> mismatch_scores,
    hn::Vec<p_simd_tag> wildcard_scores,
    hn::Vec<p_simd_tag> wildcard_ranks) noexcept
{
    constexpr bool k_is_dual_affine =
        p_mode == ksw_aligner_mode::dual_affine;
    using partial_score_type = hn::TFromD<p_simd_tag>;
    ksw_dp_cell<p_simd_tag> cell;

    // --- Cell score ---
    if constexpr (p_is_fused_score) {
        auto target_ranks = hn::LoadU(simd_tag,
            reinterpret_cast<const partial_score_type*>(&state.target_bytes[state_offset]));
        auto query_ranks = hn::LoadU(simd_tag,
            reinterpret_cast<const partial_score_type*>(&query_reversed_aligned[state_offset]));
        auto wildcard_lanes = hn::Or(
            hn::VecFromMask(simd_tag, hn::Eq(target_ranks, wildcard_ranks)),
            hn::VecFromMask(simd_tag, hn::Eq(query_ranks, wildcard_ranks)));
        auto equal_lanes = hn::VecFromMask(simd_tag, hn::Eq(target_ranks, query_ranks));
        cell.selected_scores = hn::IfThenElse(
            hn::MaskFromVec(equal_lanes), match_scores, mismatch_scores);
        cell.selected_scores = hn::IfThenElse(
            hn::MaskFromVec(wildcard_lanes), wildcard_scores, cell.selected_scores);
    } else {
        cell.selected_scores = hn::Load(simd_tag, state.diag_score_base + state_offset);
    }

    if constexpr (!k_is_dual_affine) {
        cell.selected_scores = hn::Add(cell.selected_scores, double_gap_costs);
    }

    // --- Gap / diff state ---
    auto shifted_horizontal_gap_offsets = hn::Load(simd_tag, state.gap_e_offset_base + state_offset);
    shifted_horizontal_gap_offsets = shift_left_insert(simd_tag, shifted_horizontal_gap_offsets, horizontal_gap_carry);
    cell.shifted_left_differences = hn::Load(simd_tag, state.diff_left_base + state_offset);
    cell.shifted_left_differences = shift_left_insert(simd_tag, cell.shifted_left_differences, horizontal_difference_carry);
    cell.short_horizontal_gap_candidates = hn::Add(shifted_horizontal_gap_offsets, cell.shifted_left_differences);
    cell.current_up_differences = hn::Load(simd_tag, state.diff_up_base + state_offset);
    cell.short_vertical_gap_candidates = hn::Add(
        hn::Load(simd_tag, state.gap_f_offset_base + state_offset), cell.current_up_differences);

    cell.long_horizontal_gap_candidates = hn::Zero(simd_tag);
    cell.long_vertical_gap_candidates = hn::Zero(simd_tag);
    if constexpr (k_is_dual_affine) {
        auto shifted_long_horizontal_gap_offsets = hn::Load(simd_tag, state.gap_e2_offset_base + state_offset);
        shifted_long_horizontal_gap_offsets = shift_left_insert(simd_tag, shifted_long_horizontal_gap_offsets, long_horizontal_gap_carry);
        cell.long_horizontal_gap_candidates = hn::Add(shifted_long_horizontal_gap_offsets, cell.shifted_left_differences);
        cell.long_vertical_gap_candidates = hn::Add(
            hn::Load(simd_tag, state.gap_f2_offset_base + state_offset), cell.current_up_differences);
    }

    return cell;
}

// =============================================================================
// reduce_ksw_dp_maximum — reduce per-lane maxima into the result state.
// =============================================================================

/** Reduce one vector of recurrence candidates to the selected cell scores. */
template <ksw_aligner_mode p_mode, class p_simd_tag>
[[always_inline]] inline void reduce_ksw_dp_maximum(
    p_simd_tag simd_tag, ksw_dp_cell<p_simd_tag>& cell,
    hn::Vec<p_simd_tag> maximum_plausible_scores,
    hn::Vec<p_simd_tag> match_scores) noexcept
{
    constexpr bool k_is_dual_affine =
        p_mode == ksw_aligner_mode::dual_affine;

    cell.selected_scores = hn::Max(cell.selected_scores, cell.short_horizontal_gap_candidates);
    if constexpr (!k_is_dual_affine) {
        cell.selected_scores = maximum_unsigned_bytes(simd_tag, cell.selected_scores, cell.short_vertical_gap_candidates);
        cell.selected_scores = minimum_unsigned_bytes(simd_tag, cell.selected_scores, maximum_plausible_scores);
    } else {
        cell.selected_scores = hn::Max(cell.selected_scores, cell.short_vertical_gap_candidates);
        cell.selected_scores = hn::Max(cell.selected_scores, cell.long_horizontal_gap_candidates);
        cell.selected_scores = hn::Max(cell.selected_scores, cell.long_vertical_gap_candidates);
        cell.selected_scores = hn::Min(cell.selected_scores, match_scores);
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
    p_simd_tag simd_tag, int state_offset,
    ksw_dp_cell<p_simd_tag>& cell,
    p_state_type& HWY_RESTRICT state,
    hn::Vec<p_simd_tag> gap_open_scores,
    hn::Vec<p_simd_tag> short_gap_open_scores,
    hn::Vec<p_simd_tag> long_gap_open_scores,
    hn::Vec<p_simd_tag> short_gap_costs,
    hn::Vec<p_simd_tag> long_gap_costs,
    hn::Vec<p_simd_tag> zero_scores) noexcept
{
    constexpr bool k_is_dual_affine =
        p_mode == ksw_aligner_mode::dual_affine;

    hn::Store(hn::Sub(cell.selected_scores, cell.shifted_left_differences), simd_tag,
              state.diff_up_base + state_offset);
    hn::Store(hn::Sub(cell.selected_scores, cell.current_up_differences), simd_tag,
              state.diff_left_base + state_offset);

    if constexpr (!k_is_dual_affine) {
        auto single_gap_open_threshold = hn::Sub(cell.selected_scores, gap_open_scores);
        cell.short_horizontal_gap_candidates = hn::Sub(cell.short_horizontal_gap_candidates, single_gap_open_threshold);
        cell.short_vertical_gap_candidates = hn::Sub(cell.short_vertical_gap_candidates, single_gap_open_threshold);
        hn::Store(hn::Max(cell.short_horizontal_gap_candidates, zero_scores), simd_tag,
                  state.gap_e_offset_base + state_offset);
        hn::Store(hn::Max(cell.short_vertical_gap_candidates, zero_scores), simd_tag,
                  state.gap_f_offset_base + state_offset);
    } else {
        auto short_gap_open_threshold = hn::Sub(cell.selected_scores, short_gap_open_scores);
        cell.short_horizontal_gap_candidates = hn::Sub(cell.short_horizontal_gap_candidates, short_gap_open_threshold);
        cell.short_vertical_gap_candidates = hn::Sub(cell.short_vertical_gap_candidates, short_gap_open_threshold);
        auto long_gap_open_threshold = hn::Sub(cell.selected_scores, long_gap_open_scores);
        cell.long_horizontal_gap_candidates = hn::Sub(cell.long_horizontal_gap_candidates, long_gap_open_threshold);
        cell.long_vertical_gap_candidates = hn::Sub(cell.long_vertical_gap_candidates, long_gap_open_threshold);
        hn::Store(hn::Sub(hn::Max(cell.short_horizontal_gap_candidates, zero_scores), short_gap_costs),
                  simd_tag, state.gap_e_offset_base + state_offset);
        hn::Store(hn::Sub(hn::Max(cell.short_vertical_gap_candidates, zero_scores), short_gap_costs),
                  simd_tag, state.gap_f_offset_base + state_offset);
        hn::Store(hn::Sub(hn::Max(cell.long_horizontal_gap_candidates, zero_scores), long_gap_costs),
                  simd_tag, state.gap_e2_offset_base + state_offset);
        hn::Store(hn::Sub(hn::Max(cell.long_vertical_gap_candidates, zero_scores), long_gap_costs),
                  simd_tag, state.gap_f2_offset_base + state_offset);
    }
}

}  // namespace target
}  // namespace bioseq::simd
