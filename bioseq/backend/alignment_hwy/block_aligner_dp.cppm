/**
 * @file block_aligner_dp.cppm
 * @brief Implements adaptive-block SIMD dynamic programming and trace storage.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <concepts>
#include <tuple>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.highway.sources.block_aligner_dp;

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.highway.sources.simd_utils;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.highway.sources.alignment_utils;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// Constants
// =============================================================================

/// Block shift step retained by every supported SIMD lane shape.
inline constexpr size_t k_block_shift_step = 8;

/// Consecutive x-drop confirmations before termination.
inline constexpr size_t k_x_drop_confirmation_count = 2;

/// Whether block shrinking is enabled.
inline constexpr bool k_block_shrink_enabled = true;

/// Elements inspected at the end of each border for shrink decision.
inline constexpr size_t k_block_shrink_suffix_length = k_block_shift_step / 4;

/// Baseline bias for all i16 DP cells. True delta = cell - k_block_score_bias.
inline constexpr int16_t k_block_score_bias = static_cast<int16_t>(1 << 14);

/// Minimum representable relative score (saturating floor).
inline constexpr int16_t k_block_score_minimum = 0;

/// SIMD tag capped at the validated eight- or sixteen-lane geometry.
using block_aligner_simd_tag = hn::CappedTag<int16_t, 16>;

/** Exact work counters collected by one adaptive-block execution. */
struct block_core_stats {
    std::uint64_t vector_batches = 0;  ///< DP vector batches evaluated.
    std::uint64_t cells = 0;  ///< DP cells evaluated.
    std::uint64_t diagonal_certificate_vector_batches = 0;  ///< Scan batches.
    std::uint64_t diagonal_certificate_symbols = 0;  ///< Symbols scanned.
    std::uint64_t grow_events = 0;  ///< Adaptive growth events.
    std::uint64_t shrink_events = 0;  ///< Adaptive shrink events.
    bool is_saturated = false;  ///< Whether a work counter saturated.
    bool is_x_drop_terminated = false;  ///< Whether X-drop terminated the run.
    bool is_diagonal_certified = false;  ///< Whether the diagonal fast path won.

    /** Saturating addition using the enclosing record's status flag. */
    void add(std::uint64_t& destination, std::uint64_t increment = 1) noexcept {
        block_aligner_execution_stats::add(destination, increment, is_saturated);
    }

    /** Multiply two factors safely and add the product to a counter. */
    void add_product(
            std::uint64_t& destination,
            std::uint64_t first,
            std::uint64_t second) noexcept {
        if (first != 0
            && second > (std::numeric_limits<std::uint64_t>::max)() / first) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            is_saturated = true;
            return;
        }
        add(destination, first * second);
    }
};

/** Abort after logging a violated internal adaptive-block invariant. */
[[noreturn]] inline void abort_block_aligner_invariant(
        std::string_view detail) {
    bioseq_log<logging_level::error>(
        "Block Aligner invariant failure: {}", detail);
    std::abort();
}

// =============================================================================
// Scalar helpers
// =============================================================================

/** Clamp a scalar score to the signed sixteen-bit DP domain. */
constexpr int16_t clamp_block_score_to_int16(int32_t score) {
    return static_cast<int16_t>(
        std::min<int32_t>(
            std::max<int32_t>(
                score, (std::numeric_limits<int16_t>::min)()),
            (std::numeric_limits<int16_t>::max)()));
}

/** Return the ceiling of a non-negative integer division. */
constexpr size_t divide_block_extent_round_up(
        size_t numerator,
        size_t denominator) {
    return (numerator + denominator - 1) / denominator;
}

// =============================================================================
// SIMD helpers
// =============================================================================

/// Shift a vector by half its validated lane count using adjacent vectors.
template<class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> shift_block_vector_half(
        p_simd_tag simd_tag,
        hn::Vec<p_simd_tag> lower_source,
        hn::Vec<p_simd_tag> upper_source) {
    return hn::ConcatLowerUpper(simd_tag, lower_source, upper_source);
}

// =============================================================================
// block_padded_bytes — internal padded byte buffer for SIMD-safe access
// =============================================================================

/// Padded sequence buffer: [NULL | byte_0..byte_{sequence_length-1} | NULL * padding].
/// The one-byte prefix prevents underread in shift_lanes_left_with_predecessor.
/// The suffix prevents SIMD overread.
/// Bytes are stored as alphabet rank values (0-based).
///
/// Storage is non-owning and must not outlive the enclosing alignment arena.
class block_padded_bytes {
public:
    /** Construct an empty padded input view. */
    block_padded_bytes() = default;

    /// Encode a canonical logical-rank view into sentinel-padded storage.
    template<bool p_is_reversed, bool p_should_write_reversed, int p_alphabet_dimension,
             sequence_alphabet p_alphabet>
    void encode_region(
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            size_t region_begin,
            size_t region_end,
            size_t block_size) {
        length_ = region_end - region_begin;
        const size_t total = 1 + length_ + block_size;
        data_ = context::current().alloc_align_array<uint8_t, 64>(total + 64);
        const uint8_t null_byte = static_cast<uint8_t>(p_alphabet_dimension);
        data_[0] = null_byte;
        sequence.template copy_ranks<
            p_is_reversed, p_should_write_reversed>(
                data_.subspan(1, length_), region_begin, region_end);
        std::memset(data_.data() + 1 + length_, null_byte, block_size);
    }

    /// Return the byte at a one-based padded index. Index zero is the sentinel.
    [[nodiscard]] uint8_t byte_at(size_t index) const { return data_[index]; }

    /// Pointer for SIMD loads at position i in padded buffer.
    [[nodiscard]] const uint8_t* data_at(size_t index) const {
        return data_.data() + index;
    }

    /// Original (unpadded) sequence length.
    [[nodiscard]] size_t length() const { return length_; }

private:
    std::span<uint8_t> data_;
    size_t length_ = 0;
};

// =============================================================================
// block_aligned_buffer — SIMD-aligned i16 buffer
// =============================================================================

/// SIMD-aligned i16 scratch buffer owned by the current alignment arena.
class block_aligned_buffer {
public:
    /** Construct an empty aligned buffer. */
    block_aligned_buffer() = default;

    /** Allocate the requested scalar capacity from the alignment arena. */
    explicit block_aligned_buffer(size_t capacity)
        : storage_(context::current().alloc_align_array<int16_t, 64>(capacity))
        , capacity_(capacity) {}

    /// Fill the active prefix with the DP minimum score.
    template<class p_simd_tag>
    void clear(p_simd_tag simd_tag, size_t count) {
        const auto minimum_scores = hn::Set(simd_tag, k_block_score_minimum);
        const size_t lane_count = hn::Lanes(simd_tag);
        for (size_t i = 0; i < count; i += lane_count) {
            hn::Store(minimum_scores, simd_tag, storage_.data() + i);
        }
    }

    /// Copy one vector from another buffer at the same offset.
    template<class p_simd_tag>
    void copy_vector_from(
            p_simd_tag simd_tag,
            const block_aligned_buffer& other,
            size_t index) {
        hn::Store(
            hn::Load(simd_tag, other.data() + index),
            simd_tag,
            data() + index);
    }

    /// Copy one vector between two offsets in this buffer.
    template<class p_simd_tag>
    void copy_vector(
            p_simd_tag simd_tag,
            size_t destination,
            size_t source) {
        hn::Store(
            hn::Load(simd_tag, data() + source),
            simd_tag,
            data() + destination);
    }

    /// Return one scalar element.
    [[nodiscard]] int16_t at(size_t index) const { return storage_[index]; }

    /// Store one scalar element.
    void set(size_t index, int16_t value) { storage_[index] = value; }

    /// Return the mutable backing pointer.
    [[nodiscard]] int16_t* data() { return storage_.data(); }

    /// Return the immutable backing pointer.
    [[nodiscard]] const int16_t* data() const { return storage_.data(); }

private:
    std::span<int16_t> storage_;
    size_t capacity_ = 0;
};

// =============================================================================
// block_trace — trace data for CIGAR generation
// =============================================================================

/// CIGAR trace buffer. Five parallel arrays — the `primary_trace_words_` / `secondary_trace_words_`
/// pair can reach multi-MiB for long fill alignments.
///
/// Storage is non-owning and belongs to the current alignment arena. Only the
/// packed direction bits require initialization. DP writes the other arrays
/// before traceback reads them.
class block_trace {
public:
    /// Packed trace word holding two decision bits per SIMD lane.
    using trace_type = uint32_t;

    /** Construct an empty trace buffer for score-only alignment. */
    block_trace() = default;

    /** Allocate trace storage for one admitted alignment geometry. */
    block_trace(
            size_t query_length,
            size_t reference_length,
            size_t maximum_block_size)
        : query_length_(query_length), reference_length_(reference_length) {
        const size_t len = query_length + reference_length + 2;
        const size_t lane_count = hn::Lanes(block_aligner_simd_tag{});
        if (maximum_block_size > (std::numeric_limits<size_t>::max)() / 2
            || len > (std::numeric_limits<size_t>::max)()
                - maximum_block_size * 2) {
            abort_block_aligner_invariant("trace capacity arithmetic overflow");
        }
        const size_t trace_columns = len + maximum_block_size * 2;
        const size_t trace_rows = maximum_block_size / lane_count;
        if (trace_rows != 0
            && trace_columns > (std::numeric_limits<size_t>::max)() / trace_rows) {
            abort_block_aligner_invariant("trace capacity arithmetic overflow");
        }
        trace_capacity_ = trace_rows * trace_columns;
        if (trace_capacity_
                > (std::numeric_limits<size_t>::max)() - 64) {
            abort_block_aligner_invariant("trace capacity guard overflow");
        }
        const size_t trace_cap = trace_capacity_ + 64;
        const size_t right_cap = divide_block_extent_round_up(len * 2, 64);
        const size_t block_cap = len * 4;
        auto& ctx = context::current();
        primary_trace_words_       = ctx.alloc_align_array<trace_type>(trace_cap);
        secondary_trace_words_      = ctx.alloc_align_array<trace_type>(trace_cap);
        block_directions_       = ctx.alloc_align_array<uint64_t>(right_cap);
        block_origins_ = ctx.alloc_align_array<uint32_t>(block_cap);
        block_extents_  = ctx.alloc_align_array<uint16_t>(block_cap);
        std::memset(block_directions_.data(), 0, right_cap * sizeof(uint64_t));
    }

    /** Reset counters and dimensions while retaining allocated storage. */
    void clear(size_t query_length, size_t reference_length) {
        std::memset(block_directions_.data(), 0, block_directions_.size() * sizeof(uint64_t));
        trace_word_count_ = 0;
        block_count_ = 0;
        checkpoint_trace_word_count_ = 0;
        checkpoint_block_count_ = 0;
        query_length_ = query_length;
        reference_length_ = reference_length;
    }

    /** Append one primary and secondary packed trace word. */
    void append_trace_words(
            trace_type primary_word,
            trace_type secondary_word) {
        // Constructor-time capacity validation covers every admitted
        // trace word. Keep only a debug assertion in the hottest DP loop;
        // fail-closed runtime checks remain at capacity construction and at
        // the rectangle-level skip boundary below.
        HWY_DASSERT_M(trace_word_count_ < trace_capacity_,
                      "prevalidated Block trace capacity exceeded");
        primary_trace_words_[trace_word_count_] = primary_word;
        secondary_trace_words_[trace_word_count_] = secondary_word;
        ++trace_word_count_;
    }

    /** Append one traced block rectangle and its traversal direction. */
    void append_block(
            size_t row_start,
            size_t column_start,
            size_t width,
            size_t height,
            bool is_right_block) {
        const bool is_block_slot_valid = block_origins_.size() >= 2
            && block_extents_.size() >= 2
            && block_count_ <= (block_origins_.size() - 2) / 2
            && block_count_ <= (block_extents_.size() - 2) / 2;
        const size_t word = block_count_ / 64;
        if (!is_block_slot_valid || word >= block_directions_.size()
            || row_start > std::numeric_limits<std::uint32_t>::max()
            || column_start > std::numeric_limits<std::uint32_t>::max()
            || width > std::numeric_limits<std::uint16_t>::max()
            || height > std::numeric_limits<std::uint16_t>::max()) {
            abort_block_aligner_invariant("trace block capacity/encoding exceeded");
        }
        block_origins_[block_count_ * 2] =
            static_cast<uint32_t>(row_start);
        block_origins_[block_count_ * 2 + 1] =
            static_cast<uint32_t>(column_start);
        block_extents_[block_count_ * 2]      = static_cast<uint16_t>(height);
        block_extents_[block_count_ * 2 + 1]  = static_cast<uint16_t>(width);
        const size_t bit  = block_count_ % 64;
        block_directions_[word] = (block_directions_[word] & ~(1ULL << bit))
                     | (static_cast<uint64_t>(is_right_block) << bit);
        ++block_count_;
    }

    /** Advance over trace words for cells skipped by early termination. */
    void skip_trace_words(size_t count) {
        if (trace_word_count_ > trace_capacity_
            || count > trace_capacity_ - trace_word_count_) {
            abort_block_aligner_invariant("trace skip exceeded capacity");
        }
        trace_word_count_ += count;
    }

    /// Save the current trace append positions.
    void save_checkpoint() { checkpoint_trace_word_count_ = trace_word_count_; checkpoint_block_count_ = block_count_; }

    /// Restore the most recently saved trace append positions.
    void restore_checkpoint() { trace_word_count_ = checkpoint_trace_word_count_; block_count_ = checkpoint_block_count_; }

    /// Return primary packed trace storage.
    [[nodiscard]] std::span<const trace_type> primary_trace_words() const { return primary_trace_words_; }
    /// Return secondary packed trace storage.
    [[nodiscard]] std::span<const trace_type> secondary_trace_words() const { return secondary_trace_words_; }
    /// Return packed block-direction bits.
    [[nodiscard]] std::span<const uint64_t>  block_directions() const { return block_directions_; }
    /// Return interleaved block row and column origins.
    [[nodiscard]] std::span<const uint32_t>  block_origins() const { return block_origins_; }
    /// Return interleaved block height and width values.
    [[nodiscard]] std::span<const uint16_t>  block_extents() const { return block_extents_; }
    /// Return the number of written trace words.
    [[nodiscard]] size_t trace_word_count() const { return trace_word_count_; }
    /// Return the number of written block records.
    [[nodiscard]] size_t block_count() const { return block_count_; }
    /// Return the admitted query length.
    [[nodiscard]] size_t query_length() const { return query_length_; }
    /// Return the admitted reference length.
    [[nodiscard]] size_t reference_length() const { return reference_length_; }

private:
    std::span<trace_type> primary_trace_words_;
    std::span<trace_type> secondary_trace_words_;
    std::span<uint64_t>  block_directions_;
    std::span<uint32_t>  block_origins_;
    std::span<uint16_t>  block_extents_;
    size_t trace_word_count_ = 0, block_count_ = 0;
    size_t checkpoint_trace_word_count_ = 0, checkpoint_block_count_ = 0;
    size_t query_length_ = 0, reference_length_ = 0;
    size_t trace_capacity_ = 0;
};

// =============================================================================
// Trace LUT — compile-time lookup table for traceback
// =============================================================================

/** Dynamic-programming table selected by a traceback transition. */
enum class block_trace_table : uint8_t { diagonal = 0, column = 1, row = 2 };

/** One decoded traceback transition. */
struct block_trace_lookup_entry {
    block_cigar_operation operation;  ///< Operation emitted by the transition.
    size_t query_step;  ///< Query symbols consumed.
    size_t reference_step;  ///< Reference symbols consumed.
    block_trace_table next_table;  ///< Table used by the preceding transition.
};

/** Build the complete trace-word decoding table at compile time. */
consteval std::array<std::array<block_trace_lookup_entry, 64>, 2> build_block_trace_lookup() {
    std::array<std::array<block_trace_lookup_entry, 64>, 2> trace_lookup{};
    for (size_t orientation_index = 0; orientation_index < 2; ++orientation_index)
        for (size_t code_index = 0; code_index < 64; ++code_index)
            trace_lookup[orientation_index][code_index] = {block_cigar_operation::deletion, 0, 1, block_trace_table::diagonal};

    for (size_t is_right_block = 0; is_right_block < 2; ++is_right_block) {
        for (size_t primary_trace_code = 0; primary_trace_code < 4; ++primary_trace_code) {
            for (size_t secondary_trace_code = 0; secondary_trace_code < 4; ++secondary_trace_code) {
                for (size_t table_index = 0; table_index < 3; ++table_index) {
                    const size_t index = (primary_trace_code << 4) | (secondary_trace_code << 2) | table_index;
                    block_trace_lookup_entry entry{};
                    if (is_right_block == 1) {
                        switch (table_index) {
                        case 0: // diagonal table
                            if (primary_trace_code == 0b00) {
                                entry = {block_cigar_operation::match, 1, 1, block_trace_table::diagonal};
                            } else if (primary_trace_code & 0b01) {
                                entry = (secondary_trace_code & 0b01)
                                    ? block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::diagonal}
                                    : block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::column};
                            } else {
                                entry = (secondary_trace_code & 0b10)
                                    ? block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::diagonal}
                                    : block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::row};
                            }
                            break;
                        case 1: // column-gap table
                            entry = (secondary_trace_code & 0b01)
                                ? block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::diagonal}
                                : block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::column};
                            break;
                        case 2: // row-gap table
                            entry = (secondary_trace_code & 0b10)
                                ? block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::diagonal}
                                : block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::row};
                            break;
                        }
                    } else {
                        // Down block: column/row and insertion/deletion swap.
                        switch (table_index) {
                        case 0: // diagonal table
                            if (primary_trace_code == 0b00) {
                                entry = {block_cigar_operation::match, 1, 1, block_trace_table::diagonal};
                            } else if (primary_trace_code & 0b01) {
                                entry = (secondary_trace_code & 0b01)
                                    ? block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::diagonal}
                                    : block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::row};
                            } else {
                                entry = (secondary_trace_code & 0b10)
                                    ? block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::diagonal}
                                    : block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::column};
                            }
                            break;
                        case 1: // column-gap table acts as row-gap
                            entry = (secondary_trace_code & 0b10)
                                ? block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::diagonal}
                                : block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::column};
                            break;
                        case 2: // row-gap table acts as column-gap
                            entry = (secondary_trace_code & 0b01)
                                ? block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::diagonal}
                                : block_trace_lookup_entry{block_cigar_operation::insertion, 1, 0, block_trace_table::row};
                            break;
                        }
                    }
                    trace_lookup[is_right_block][index] = entry;
                }
            }
        }
    }
    return trace_lookup;
}

/** Complete lookup from packed trace decisions to traceback transitions. */
inline constexpr auto k_block_trace_lookup = build_block_trace_lookup();

// =============================================================================
// Adaptive-block working memory
// =============================================================================

/** Arena-backed borders, checkpoints, and trace storage for one alignment. */
struct block_aligner_scratch {
    block_trace trace;  ///< Optional packed traceback storage.
    block_aligned_buffer diagonal_column, gap_column, diagonal_row, gap_row;  ///< Active borders.
    block_aligned_buffer diagonal_column_checkpoint, gap_column_checkpoint, diagonal_row_checkpoint, gap_row_checkpoint;  ///< Restorable borders.
    block_aligned_buffer temporary_buffer_one, temporary_buffer_two;  ///< SIMD shift scratch.

    /** Allocate working storage for an admitted block geometry. */
    block_aligner_scratch(
            size_t query_length,
            size_t reference_length,
            size_t maximum_block_size,
            bool is_trace_enabled)
        : trace(is_trace_enabled
            ? block_trace(query_length, reference_length, maximum_block_size)
            : block_trace())
        , diagonal_column(maximum_block_size)
        , gap_column(maximum_block_size)
        , diagonal_row(maximum_block_size)
        , gap_row(maximum_block_size)
        , diagonal_column_checkpoint(maximum_block_size)
        , gap_column_checkpoint(maximum_block_size)
        , diagonal_row_checkpoint(maximum_block_size)
        , gap_row_checkpoint(maximum_block_size)
        , temporary_buffer_one(64), temporary_buffer_two(64) {}

    /** Reset all active borders and optional trace positions. */
    template<class p_simd_tag>
    void clear(
            p_simd_tag simd_tag,
            size_t query_length,
            size_t reference_length,
            size_t maximum_block_size,
            bool is_trace_enabled) {
        // Checkpoint restoration requires the complete admitted border
        // geometry to begin at the DP minimum score.
        if (is_trace_enabled) trace.clear(query_length, reference_length);
        diagonal_column.clear(simd_tag, maximum_block_size);
        gap_column.clear(simd_tag, maximum_block_size);
        diagonal_row.clear(simd_tag, maximum_block_size);
        gap_row.clear(simd_tag, maximum_block_size);
        diagonal_column_checkpoint.clear(simd_tag, maximum_block_size);
        gap_column_checkpoint.clear(simd_tag, maximum_block_size);
        diagonal_row_checkpoint.clear(simd_tag, maximum_block_size);
        gap_row_checkpoint.clear(simd_tag, maximum_block_size);
        const size_t lane_count = hn::Lanes(simd_tag);
        temporary_buffer_one.clear(simd_tag, lane_count);
        temporary_buffer_two.clear(simd_tag, lane_count);
    }
};

// =============================================================================
// Prefix scan — O(log lane_count) parallel prefix max with cumulative gap cost
// =============================================================================

/// Precompute the accumulated extension costs used by the prefix scan.
template<class p_simd_tag>
auto make_block_prefix_scan_constants(
        p_simd_tag simd_tag,
        hn::Vec<p_simd_tag> gap_extension_scores)
    -> std::pair<hn::Vec<p_simd_tag>, hn::Vec<p_simd_tag>>
{
    const size_t lane_count = hn::Lanes(simd_tag);
    auto one_lane_costs =
        shift_lanes_left_zero<1>(simd_tag, gap_extension_scores);
    one_lane_costs = hn::SaturatedAdd(
        one_lane_costs, gap_extension_scores);
    auto two_lane_costs =
        shift_lanes_left_zero<2>(simd_tag, one_lane_costs);
    two_lane_costs = hn::SaturatedAdd(two_lane_costs, one_lane_costs);
    auto four_lane_costs =
        shift_lanes_left_zero<4>(simd_tag, two_lane_costs);
    four_lane_costs = hn::SaturatedAdd(four_lane_costs, two_lane_costs);

    if (lane_count > 8) {
        HWY_ALIGN int16_t lane_values[hn::MaxLanes(simd_tag)];
        hn::Store(four_lane_costs, simd_tag, lane_values);
        const int16_t half_last = lane_values[lane_count / 2 - 1];
        HWY_ALIGN int16_t corrected_costs[hn::MaxLanes(simd_tag)];
        for (size_t i = 0; i < lane_count / 2; ++i) {
            corrected_costs[i] = lane_values[i];
        }
        for (size_t i = lane_count / 2; i < lane_count; ++i) {
            corrected_costs[i] = clamp_block_score_to_int16(
                static_cast<int32_t>(half_last) + static_cast<int32_t>(lane_values[i]));
        }
        return {hn::Load(simd_tag, corrected_costs), four_lane_costs};
    }
    return {four_lane_costs, four_lane_costs};
}

/// Parallel prefix scan: result[i] = max_{k=0..i} (input[k] + gap * (i-k)).
/// Uses lane-independent shifts (ShiftLeftBytes), NOT SlideUpLanes.
template<class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> scan_block_prefix(
        p_simd_tag simd_tag,
        hn::Vec<p_simd_tag> input_scores,
        hn::Vec<p_simd_tag> gap_costs,
        hn::Vec<p_simd_tag> lane_offsets)
{
    const size_t lane_count = hn::Lanes(simd_tag);
    auto one_lane_scores = shift_lanes_left_zero<1>(simd_tag, input_scores);
    one_lane_scores = hn::SaturatedAdd(one_lane_scores, gap_costs);
    one_lane_scores = hn::Max(input_scores, one_lane_scores);

    const auto two_lane_gap_costs = hn::ShiftLeft<1>(gap_costs);
    auto two_lane_scores =
        shift_lanes_left_zero<2>(simd_tag, one_lane_scores);
    two_lane_scores = hn::SaturatedAdd(
        two_lane_scores, two_lane_gap_costs);
    two_lane_scores = hn::Max(one_lane_scores, two_lane_scores);

    const auto four_lane_gap_costs = hn::ShiftLeft<2>(gap_costs);
    auto four_lane_scores =
        shift_lanes_left_zero<4>(simd_tag, two_lane_scores);
    four_lane_scores = hn::SaturatedAdd(
        four_lane_scores, four_lane_gap_costs);
    four_lane_scores = hn::Max(two_lane_scores, four_lane_scores);

    if (lane_count > 8) {
        HWY_ALIGN int16_t lane_values[hn::MaxLanes(simd_tag)];
        hn::Store(four_lane_scores, simd_tag, lane_values);
        const int16_t lower_last = lane_values[lane_count / 2 - 1];
        HWY_ALIGN int16_t lane_offset_values[hn::MaxLanes(simd_tag)];
        hn::Store(lane_offsets, simd_tag, lane_offset_values);
        HWY_ALIGN int16_t corrected_values[hn::MaxLanes(simd_tag)];
        for (size_t i = 0; i < lane_count / 2; ++i) corrected_values[i] = k_block_score_minimum;
        for (size_t i = lane_count / 2; i < lane_count; ++i) {
            corrected_values[i] = clamp_block_score_to_int16(
                static_cast<int32_t>(lower_last)
                + static_cast<int32_t>(lane_offset_values[i]));
        }
        return hn::Max(
            four_lane_scores,
            hn::Load(simd_tag, corrected_values));
    }
    return four_lane_scores;
}

// =============================================================================
// Trace recording — compress DP cell decisions into bitmasks
// =============================================================================

/// Spread 16 low bits of `low_bits` into the even bit positions of a 32-bit
/// result (bits 0,2,4,...,30) and the 16 low bits of `high_bits` into the odd
/// bit positions (bits 1,3,5,...,31). Used by the trace recorder to
/// pack two parallel mask vectors into a single trace_type word.
HWY_INLINE constexpr uint32_t interleave_low_trace_bits(
        uint32_t low_bits,
        uint32_t high_bits) {
    const auto spread_bits = [](uint32_t bits) -> uint32_t {
        bits &= 0xFFFFu;
        bits = (bits | (bits << 8)) & 0x00FF00FFu;
        bits = (bits | (bits << 4)) & 0x0F0F0F0Fu;
        bits = (bits | (bits << 2)) & 0x33333333u;
        bits = (bits | (bits << 1)) & 0x55555555u;
        return bits;
    };
    return spread_bits(low_bits) | (spread_bits(high_bits) << 1);
}

/** Pack the affine-gap predecessor decisions for one SIMD vector. */
template<class p_simd_tag>
HWY_INLINE void record_block_trace(
        p_simd_tag simd_tag,
        block_trace& trace,
        hn::Vec<p_simd_tag> diagonal_scores,
        hn::Vec<p_simd_tag> column_gap_scores,
        hn::Vec<p_simd_tag> row_gap_scores,
        hn::Vec<p_simd_tag> opened_column_gap_scores,
        hn::Vec<p_simd_tag> opened_row_gap_scores,
        hn::Vec<p_simd_tag>& previous_row_trace,
        [[maybe_unused]] int16_t relative_zero)
{
    [[maybe_unused]] const size_t lane_count = hn::Lanes(simd_tag);
    // trace_type is uint32_t (32 bits). With 2 bits per lane, max lane_count = 16.
    // AVX-512 (lane_count=32) would need uint64_t traces.
    HWY_DASSERT_M(lane_count <= 16, "Trace bitmask overflow: lane_count > 16 not supported");

    // BitsFromMask compresses one predecessor decision per active lane.
    const auto diagonal_equals_column_mask = hn::Eq(diagonal_scores, column_gap_scores);
    const auto diagonal_equals_row_mask = hn::Eq(diagonal_scores, row_gap_scores);
    const uint32_t diagonal_column_bits = static_cast<uint32_t>(hn::BitsFromMask(simd_tag, diagonal_equals_column_mask));
    const uint32_t diagonal_row_bits = static_cast<uint32_t>(hn::BitsFromMask(simd_tag, diagonal_equals_row_mask));
    const block_trace::trace_type primary_trace_word =
        interleave_low_trace_bits(diagonal_column_bits, diagonal_row_bits);

    auto current_row_trace = hn::VecFromMask(
        simd_tag,
        hn::Eq(row_gap_scores, opened_row_gap_scores));
    auto shifted_row_trace = shift_lanes_left_with_predecessor(
        simd_tag, current_row_trace, previous_row_trace);
    const auto column_open_mask = hn::Eq(column_gap_scores, opened_column_gap_scores);
    const auto shifted_row_open_mask = hn::MaskFromVec(shifted_row_trace);
    const uint32_t column_open_bits = static_cast<uint32_t>(hn::BitsFromMask(simd_tag, column_open_mask));
    const uint32_t shifted_row_open_bits = static_cast<uint32_t>(hn::BitsFromMask(simd_tag, shifted_row_open_mask));
    const block_trace::trace_type secondary_trace_word =
        interleave_low_trace_bits(column_open_bits, shifted_row_open_bits);

    previous_row_trace = current_row_trace;
    trace.append_trace_words(primary_trace_word, secondary_trace_word);
}

// =============================================================================
// Rectangular DP update
// =============================================================================

/// Compute one rectangular region of the adaptive-block DP matrix.
/// For downward shifts, the caller swaps query/reference and border buffers.
/// Returns (maximum_scores, maximum_row_offsets, maximum_column_offsets) per lane.
template<bool p_is_trace_enabled, bool p_is_x_drop_enabled, bool p_should_collect_stats,
         sequence_alphabet p_alphabet, class p_simd_tag,
         size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
HWY_INLINE auto place_alignment_block(
    p_simd_tag simd_tag,
    const scoring_matrix<p_alphabet>& score_matrix,
    int8_t negative_gap_open,
    int8_t negative_gap_extension,
    const block_padded_bytes& query,
    const block_padded_bytes& reference,
    block_trace& trace,
    size_t row_start,
    size_t column_start,
    size_t width,
    size_t height,
    int16_t* HWY_RESTRICT diagonal_column,
    int16_t* HWY_RESTRICT gap_column,
    int16_t* HWY_RESTRICT diagonal_row,
    int16_t* HWY_RESTRICT gap_row,
    hn::Vec<p_simd_tag> diagonal_corner,
    int16_t relative_zero,
    [[maybe_unused]] bool is_right_block,
    block_core_stats& core_stats
) -> std::tuple<hn::Vec<p_simd_tag>, hn::Vec<p_simd_tag>, hn::Vec<p_simd_tag>>
{
    const size_t lane_count = hn::Lanes(simd_tag);

    const auto gap_open_scores = hn::Set(simd_tag, static_cast<int16_t>(negative_gap_open));
    const auto gap_extension_scores = hn::Set(simd_tag, static_cast<int16_t>(negative_gap_extension));
    auto [accumulated_gap_extension, prefix_scan_offsets] =
        make_block_prefix_scan_constants(simd_tag, gap_extension_scores);
    auto maximum_scores = hn::Set(simd_tag, k_block_score_minimum);
    auto maximum_row_offsets = hn::Set(simd_tag, static_cast<int16_t>(0));
    auto maximum_column_offsets = hn::Set(simd_tag, static_cast<int16_t>(0));

    if (width == 0 || height == 0) {
        return {maximum_scores, maximum_row_offsets, maximum_column_offsets};
    }

    // Adjust opened row gaps before the accumulated extension scan.
    const auto gap_open_adjustment = hn::SaturatedSub(gap_open_scores, gap_extension_scores);
    const auto minimum_scores = hn::Set(simd_tag, k_block_score_minimum);

    hn::Vec<p_simd_tag> diagonal_scores{}, row_gap_scores{};

    // Only the first block can contain the DP origin.
    const bool can_have_origin = (row_start == 0 && column_start == 0);

    size_t processed_columns = 0;
    for (size_t j = 0; j < width; ++j) {
        auto previous_row_gap_scores = minimum_scores;
        diagonal_scores = minimum_scores;
        row_gap_scores = minimum_scores;
        [[maybe_unused]] auto previous_row_trace = minimum_scores;

        const uint8_t reference_rank = reference.byte_at(column_start + j);
        const auto score_lookup = load_score_lookup_tables<p_alphabet>(
            score_matrix, simd_tag, reference_rank);

        // Origin can only land on j == 0 when can_have_origin is true.
        const bool column_contains_origin = can_have_origin && (j == 0);

        for (size_t i = 0; i < height; i += lane_count) {
            auto previous_diagonal_scores = hn::Load(simd_tag, diagonal_column + i);
            auto previous_column_gap_scores = hn::Load(simd_tag, gap_column + i);

            // Diagonal: shift previous_diagonal_scores left by 1, borrow from diagonal_corner
            auto diagonal_predecessor_scores = shift_lanes_left_with_predecessor(simd_tag, previous_diagonal_scores, diagonal_corner);
            diagonal_corner = previous_diagonal_scores;

            // Score lookup (LUT pre-loaded above; no per-cell row load)
            auto scores = get_scores_with_lookup_tables<p_alphabet>(
                score_matrix,
                simd_tag,
                score_lookup,
                query.data_at(row_start + i));

            // Match/mismatch
            diagonal_scores = hn::SaturatedAdd(diagonal_predecessor_scores, scores);

            // Origin cell: insert relative_zero at lane 0 — the inner
            // condition is only reachable when `i == 0` (since
            // row_start + i == 0 requires both row_start == 0 and i == 0).
            if (column_contains_origin && i == 0) {
                diagonal_scores = replace_first_lanes(simd_tag, diagonal_scores, relative_zero);
            }

            // Column gap recurrence from the previous column border.
            auto opened_column_gap_scores = hn::SaturatedAdd(previous_diagonal_scores, gap_open_scores);
            auto column_gap_scores = hn::Max(hn::SaturatedAdd(previous_column_gap_scores, gap_extension_scores), opened_column_gap_scores);
            diagonal_scores = hn::Max(diagonal_scores, column_gap_scores);

            // Row gap via prefix scan
            auto opened_row_gap_scores = hn::SaturatedAdd(
                diagonal_scores, gap_open_adjustment);
            row_gap_scores = scan_block_prefix(
                simd_tag,
                opened_row_gap_scores,
                gap_extension_scores,
                prefix_scan_offsets);

            // Cross-vector row dependency
            row_gap_scores = hn::Max(row_gap_scores, hn::SaturatedAdd(
                broadcast_last_lane(simd_tag, previous_row_gap_scores), accumulated_gap_extension));
            diagonal_scores = hn::Max(diagonal_scores, row_gap_scores);
            previous_row_gap_scores = row_gap_scores;

            // Trace recording
            if constexpr (p_is_trace_enabled) {
                record_block_trace(
                    simd_tag,
                    trace,
                    diagonal_scores,
                    column_gap_scores,
                    row_gap_scores,
                    opened_column_gap_scores,
                    opened_row_gap_scores,
                    previous_row_trace,
                    relative_zero);
            }

            // Track per-lane maximum
            maximum_scores = hn::Max(maximum_scores, diagonal_scores);
            if constexpr (p_is_x_drop_enabled) {
                auto update_mask = hn::MaskFromVec(
                    hn::VecFromMask(simd_tag, hn::Eq(maximum_scores, diagonal_scores)));
                maximum_row_offsets = hn::IfThenElse(update_mask,
                    hn::Set(simd_tag, static_cast<int16_t>(i)), maximum_row_offsets);
                maximum_column_offsets = hn::IfThenElse(update_mask,
                    hn::Set(simd_tag, static_cast<int16_t>(j)), maximum_column_offsets);
            }

            // Store updated column borders
            hn::Store(diagonal_scores, simd_tag, diagonal_column + i);
            hn::Store(column_gap_scores, simd_tag, gap_column + i);
        }

        // End of column — update bottom border
        diagonal_corner = hn::Set(simd_tag, k_block_score_minimum);
        diagonal_row[j] = extract_last_lane(simd_tag, diagonal_scores);
        gap_row[j] = extract_last_lane(simd_tag, row_gap_scores);
        processed_columns = j + 1;

        // Early exit past both sequence ends (global alignment only)
        if constexpr (!p_is_x_drop_enabled) {
            if (row_start + height > query.length() && column_start + j >= reference.length()) {
                if constexpr (p_is_trace_enabled) {
                    trace.skip_trace_words((width - 1 - j) * (height / lane_count));
                }
                break;
            }
        }
    }

    if constexpr (p_should_collect_stats) {
        // Account only columns whose SIMD loop actually completed.  Keeping
        // this outside the per-vector hot loop makes stats collection a pair
        // of checked additions per rectangle rather than two additions per DP
        // vector, while retaining exact early-exit semantics.
        core_stats.add_product(
            core_stats.vector_batches,
            processed_columns,
            height / lane_count);
        core_stats.add_product(core_stats.cells, processed_columns, height);
    }

    return {maximum_scores, maximum_row_offsets, maximum_column_offsets};
}

// =============================================================================
// Align core helpers
// =============================================================================

/// Shift two border arrays by the fixed block step and adjust their score bias.
template<class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> shift_block_scores_and_offset(
        p_simd_tag simd_tag,
        size_t block_size,
        block_aligned_buffer& first_buffer,
        block_aligned_buffer& second_buffer,
        block_aligned_buffer& first_temporary_buffer,
        block_aligned_buffer& second_temporary_buffer,
        hn::Vec<p_simd_tag> score_offset_adjustment)
{
    const size_t lane_count = hn::Lanes(simd_tag);
    auto current_first_scores = hn::SaturatedAdd(hn::Load(simd_tag, first_buffer.data()), score_offset_adjustment);
    auto current_second_scores = hn::SaturatedAdd(hn::Load(simd_tag, second_buffer.data()), score_offset_adjustment);

    HWY_ALIGN int16_t lane_values[hn::MaxLanes(simd_tag)];
    hn::Store(current_first_scores, simd_tag, lane_values);
    const auto diagonal_corner = hn::Set(simd_tag, lane_values[k_block_shift_step - 1]);

    if (lane_count == k_block_shift_step) {
        // lane_count == STEP: shift by one full SIMD vector
        for (size_t i = 0; i + lane_count < block_size; i += lane_count) {
            auto next_first_scores = hn::SaturatedAdd(hn::Load(simd_tag, first_buffer.data() + i + lane_count), score_offset_adjustment);
            auto next_second_scores = hn::SaturatedAdd(hn::Load(simd_tag, second_buffer.data() + i + lane_count), score_offset_adjustment);
            hn::Store(next_first_scores, simd_tag, first_buffer.data() + i);
            hn::Store(next_second_scores, simd_tag, second_buffer.data() + i);
            current_first_scores = next_first_scores; current_second_scores = next_second_scores;
        }
        hn::Store(hn::Load(simd_tag, first_temporary_buffer.data()), simd_tag, first_buffer.data() + block_size - lane_count);
        hn::Store(hn::Load(simd_tag, second_temporary_buffer.data()), simd_tag, second_buffer.data() + block_size - lane_count);
    } else {
        // lane_count > STEP: use ConcatLowerUpper (shifts by lane_count/2 = STEP)
        for (size_t i = 0; i + lane_count < block_size; i += lane_count) {
            auto next_first_scores = hn::SaturatedAdd(hn::Load(simd_tag, first_buffer.data() + i + lane_count), score_offset_adjustment);
            auto next_second_scores = hn::SaturatedAdd(hn::Load(simd_tag, second_buffer.data() + i + lane_count), score_offset_adjustment);
            hn::Store(shift_block_vector_half(simd_tag, next_first_scores, current_first_scores), simd_tag, first_buffer.data() + i);
            hn::Store(shift_block_vector_half(simd_tag, next_second_scores, current_second_scores), simd_tag, second_buffer.data() + i);
            current_first_scores = next_first_scores; current_second_scores = next_second_scores;
        }
        hn::Store(shift_block_vector_half(simd_tag, hn::Load(simd_tag, first_temporary_buffer.data()), current_first_scores),
                  simd_tag, first_buffer.data() + block_size - lane_count);
        hn::Store(shift_block_vector_half(simd_tag, hn::Load(simd_tag, second_temporary_buffer.data()), current_second_scores),
                  simd_tag, second_buffer.data() + block_size - lane_count);
    }
    return diagonal_corner;
}

/// Apply a score-bias adjustment to two complete border buffers.
template<class p_simd_tag>
HWY_INLINE void offset_block_scores(
        p_simd_tag simd_tag,
        size_t block_size,
        block_aligned_buffer& first_buffer,
        block_aligned_buffer& second_buffer,
        hn::Vec<p_simd_tag> score_offset_adjustment)
{
    const size_t lane_count = hn::Lanes(simd_tag);
    for (size_t i = 0; i < block_size; i += lane_count) {
        hn::Store(hn::SaturatedAdd(hn::Load(simd_tag, first_buffer.data() + i), score_offset_adjustment),
                  simd_tag, first_buffer.data() + i);
        hn::Store(hn::SaturatedAdd(hn::Load(simd_tag, second_buffer.data() + i), score_offset_adjustment),
                  simd_tag, second_buffer.data() + i);
    }
}

/// Return the maximum score in the leading fixed-step border segment.
template<class p_simd_tag>
HWY_INLINE int16_t block_prefix_maximum(
        p_simd_tag simd_tag,
        const int16_t* buffer) {
    HWY_ALIGN int16_t lane_values[hn::MaxLanes(simd_tag)];
    hn::Store(hn::Load(simd_tag, buffer), simd_tag, lane_values);
    int16_t result = k_block_score_minimum;
    for (size_t i = 0; i < k_block_shift_step; ++i) result = std::max(result, lane_values[i]);
    return result;
}

/// Return the maximum score in the trailing shrink-decision segment.
template<class p_simd_tag>
HWY_INLINE int16_t block_suffix_maximum(
        p_simd_tag simd_tag,
        const int16_t* buffer,
        size_t buffer_length) {
    HWY_ALIGN int16_t lane_values[hn::MaxLanes(simd_tag)];
    const size_t lane_count = hn::Lanes(simd_tag);
    hn::Store(
        hn::Load(simd_tag, buffer + buffer_length - lane_count),
        simd_tag,
        lane_values);
    int16_t result = k_block_score_minimum;
    for (size_t i = lane_count - k_block_shrink_suffix_length; i < lane_count; ++i)
        result = std::max(result, lane_values[i]);
    return result;
}

// =============================================================================
// Main adaptive-block alignment loop
// =============================================================================

/** Direction selected for the next rectangular DP update. */
enum class block_direction : uint8_t { right, down, grow };

/** Execute the adaptive-block recurrence for one admitted sequence region. */
template<bool p_is_trace_enabled, bool p_is_x_drop_enabled, bool p_should_collect_stats,
         sequence_alphabet p_alphabet, class p_simd_tag,
         size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
void align_block_core(
    p_simd_tag simd_tag,
    int32_t& output_score,
    size_t& output_query_position,
    size_t& output_reference_position,
    block_aligner_scratch& scratch,
    const scoring_matrix<p_alphabet>& score_matrix,
    int8_t negative_gap_open,
    int8_t negative_gap_extension,
    const block_padded_bytes& query,
    const block_padded_bytes& reference,
    size_t minimum_block_size, size_t maximum_block_size,
    int32_t x_drop_threshold,
    block_core_stats& core_stats)
{
    const size_t lane_count = hn::Lanes(simd_tag);

    int32_t best_score = 0;
    size_t best_query_position = 0;
    size_t best_reference_position = 0;

    auto previous_direction = block_direction::grow;
    auto direction = block_direction::grow;
    size_t previous_block_size = 0;
    size_t block_size = minimum_block_size;

    int32_t score_offset = 0, maximum_score_offset = 0, previous_score_offset = 0;
    size_t growth_delay_iterations = 0;
    [[maybe_unused]] size_t x_drop_confirmation_count = 0;

    size_t block_row_start = 0, block_column_start = 0;
    size_t checkpoint_row_start = 0, checkpoint_column_start = 0;
    int32_t checkpoint_score_offset = 0;

    auto diagonal_corner = hn::Set(simd_tag, k_block_score_minimum);

    int16_t right_border_maximum = k_block_score_minimum;
    int16_t down_border_maximum = k_block_score_minimum;

    for (;;) {
        previous_score_offset = score_offset;

        auto growth_maximum_scores = hn::Set(simd_tag, k_block_score_minimum);
        [[maybe_unused]] auto growth_maximum_row_offsets = hn::Set(simd_tag, static_cast<int16_t>(0));
        [[maybe_unused]] auto growth_maximum_column_offsets = hn::Set(simd_tag, static_cast<int16_t>(0));

        hn::Vec<p_simd_tag> maximum_scores{}, maximum_row_offsets{}, maximum_column_offsets{};

        switch (direction) {
        case block_direction::right: {
            score_offset = maximum_score_offset;
            auto score_offset_adjustment = hn::Set(simd_tag, clamp_block_score_to_int16(previous_score_offset - score_offset));

            if constexpr (p_is_trace_enabled) {
                scratch.trace.append_block(block_row_start,
                    block_column_start + block_size - k_block_shift_step, k_block_shift_step, block_size, true);
            }

            offset_block_scores(simd_tag, block_size, scratch.diagonal_column, scratch.gap_column, score_offset_adjustment);

            auto diagonal_corner_input = (previous_direction == block_direction::down)
                ? hn::SaturatedAdd(diagonal_corner, score_offset_adjustment) : hn::Set(simd_tag, k_block_score_minimum);

            std::tie(maximum_scores, maximum_row_offsets, maximum_column_offsets) =
                place_alignment_block<p_is_trace_enabled, p_is_x_drop_enabled, p_should_collect_stats, p_alphabet>(
                    simd_tag, score_matrix, negative_gap_open, negative_gap_extension,
                    query, reference, scratch.trace,
                    block_row_start, block_column_start + block_size - k_block_shift_step,
                    k_block_shift_step, block_size,
                    scratch.diagonal_column.data(), scratch.gap_column.data(),
                    scratch.temporary_buffer_one.data(), scratch.temporary_buffer_two.data(),
                    diagonal_corner_input, clamp_block_score_to_int16(-score_offset + k_block_score_bias), true,
                    core_stats);

            right_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_column.data());
            diagonal_corner = shift_block_scores_and_offset(simd_tag, block_size,
                scratch.diagonal_row, scratch.gap_row,
                scratch.temporary_buffer_one, scratch.temporary_buffer_two, score_offset_adjustment);
            down_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_row.data());
            break;
        }

        case block_direction::down: {
            score_offset = maximum_score_offset;
            auto score_offset_adjustment = hn::Set(simd_tag, clamp_block_score_to_int16(previous_score_offset - score_offset));

            if constexpr (p_is_trace_enabled) {
                scratch.trace.append_block(block_row_start + block_size - k_block_shift_step,
                    block_column_start, block_size, k_block_shift_step, false);
            }

            offset_block_scores(simd_tag, block_size, scratch.diagonal_row, scratch.gap_row, score_offset_adjustment);

            auto diagonal_corner_input = (previous_direction == block_direction::right)
                ? hn::SaturatedAdd(diagonal_corner, score_offset_adjustment) : hn::Set(simd_tag, k_block_score_minimum);

            // Query/ref SWAPPED, diagonal_row<->diagonal_column, gap_row<->gap_column
            std::tie(maximum_scores, maximum_row_offsets, maximum_column_offsets) =
                place_alignment_block<p_is_trace_enabled, p_is_x_drop_enabled, p_should_collect_stats, p_alphabet>(
                    simd_tag, score_matrix, negative_gap_open, negative_gap_extension,
                    reference, query, scratch.trace,
                    block_column_start, block_row_start + block_size - k_block_shift_step,
                    k_block_shift_step, block_size,
                    scratch.diagonal_row.data(), scratch.gap_row.data(),
                    scratch.temporary_buffer_one.data(), scratch.temporary_buffer_two.data(),
                    diagonal_corner_input, clamp_block_score_to_int16(-score_offset + k_block_score_bias), false,
                    core_stats);

            down_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_row.data());
            diagonal_corner = shift_block_scores_and_offset(simd_tag, block_size,
                scratch.diagonal_column, scratch.gap_column,
                scratch.temporary_buffer_one, scratch.temporary_buffer_two, score_offset_adjustment);
            right_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_column.data());
            break;
        }

        case block_direction::grow: {
            diagonal_corner = hn::Set(simd_tag, k_block_score_minimum);
            const size_t grow_step = block_size - previous_block_size;

            if constexpr (p_is_trace_enabled) {
                scratch.trace.append_block(block_row_start + previous_block_size, block_column_start,
                    previous_block_size, grow_step, false);
            }

            // Grow DOWN: previous_block_size cols x grow_step rows
            hn::Vec<p_simd_tag> first_maximum_scores, first_row_offsets, first_column_offsets;
            std::tie(first_maximum_scores, first_row_offsets, first_column_offsets) =
                place_alignment_block<p_is_trace_enabled, p_is_x_drop_enabled, p_should_collect_stats, p_alphabet>(
                    simd_tag, score_matrix, negative_gap_open, negative_gap_extension,
                    reference, query, scratch.trace,
                    block_column_start, block_row_start + previous_block_size,
                    grow_step, previous_block_size,
                    scratch.diagonal_row.data(), scratch.gap_row.data(),
                    scratch.diagonal_column.data() + previous_block_size,
                    scratch.gap_column.data() + previous_block_size,
                    hn::Set(simd_tag, k_block_score_minimum), clamp_block_score_to_int16(-score_offset + k_block_score_bias), false,
                    core_stats);
            growth_maximum_row_offsets = first_row_offsets;
            growth_maximum_column_offsets = first_column_offsets;

            if constexpr (p_is_trace_enabled) {
                scratch.trace.append_block(block_row_start, block_column_start + previous_block_size,
                    grow_step, block_size, true);
            }

            // Grow RIGHT: grow_step cols x block_size rows
            hn::Vec<p_simd_tag> second_maximum_scores, second_row_offsets, second_column_offsets;
            std::tie(second_maximum_scores, second_row_offsets, second_column_offsets) =
                place_alignment_block<p_is_trace_enabled, p_is_x_drop_enabled, p_should_collect_stats, p_alphabet>(
                    simd_tag, score_matrix, negative_gap_open, negative_gap_extension,
                    query, reference, scratch.trace,
                    block_row_start, block_column_start + previous_block_size,
                    grow_step, block_size,
                    scratch.diagonal_column.data(), scratch.gap_column.data(),
                    scratch.diagonal_row.data() + previous_block_size,
                    scratch.gap_row.data() + previous_block_size,
                    hn::Set(simd_tag, k_block_score_minimum), clamp_block_score_to_int16(-score_offset + k_block_score_bias), true,
                    core_stats);

            right_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_column.data());
            down_border_maximum  = block_prefix_maximum(simd_tag, scratch.diagonal_row.data());
            growth_maximum_scores = first_maximum_scores;

            // Save checkpoint after grow
            for (size_t i = 0; i < block_size; i += lane_count) {
                scratch.diagonal_column_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_column, i);
                scratch.gap_column_checkpoint.copy_vector_from(simd_tag, scratch.gap_column, i);
                scratch.diagonal_row_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_row, i);
                scratch.gap_row_checkpoint.copy_vector_from(simd_tag, scratch.gap_row, i);
            }
            if constexpr (p_is_trace_enabled) { scratch.trace.save_checkpoint(); }

            maximum_scores = second_maximum_scores; maximum_row_offsets = second_row_offsets; maximum_column_offsets = second_column_offsets;
            break;
        }
        } // switch

        // --- Score update ---
        previous_direction = direction;
        int16_t maximum_score = hn::ReduceMax(simd_tag, maximum_scores);
        int16_t growth_maximum_score = hn::ReduceMax(simd_tag, growth_maximum_scores);
        int16_t current_maximum_score = std::max(maximum_score, growth_maximum_score);
        maximum_score_offset = score_offset + static_cast<int32_t>(current_maximum_score) - static_cast<int32_t>(k_block_score_bias);

        growth_delay_iterations += 1;
        bool should_grow_without_maximum = (direction == block_direction::grow);

        if (maximum_score_offset > best_score) {
            if constexpr (p_is_x_drop_enabled) {
                // Track argmax position
                int16_t lane_maximum = maximum_score;
                size_t maximum_lane = find_first_maximum_lane(simd_tag, maximum_scores, lane_maximum);
                int16_t maximum_row_offset = extract_lane(simd_tag, maximum_row_offsets, maximum_lane);
                int16_t maximum_column_offset = extract_lane(simd_tag, maximum_column_offsets, maximum_lane);
                switch (direction) {
                case block_direction::right:
                    best_query_position = block_row_start + static_cast<size_t>(maximum_row_offset) + maximum_lane;
                    best_reference_position = block_column_start + block_size - k_block_shift_step + static_cast<size_t>(maximum_column_offset);
                    break;
                case block_direction::down:
                    best_query_position = block_row_start + block_size - k_block_shift_step + static_cast<size_t>(maximum_column_offset);
                    best_reference_position = block_column_start + static_cast<size_t>(maximum_row_offset) + maximum_lane;
                    break;
                case block_direction::grow:
                    if (maximum_score >= growth_maximum_score) {
                        best_query_position = block_row_start
                            + static_cast<size_t>(maximum_row_offset) + maximum_lane;
                        best_reference_position = block_column_start + previous_block_size
                            + static_cast<size_t>(maximum_column_offset);
                    } else {
                        const size_t growth_lane = find_first_maximum_lane(
                            simd_tag, growth_maximum_scores, growth_maximum_score);
                        const int16_t growth_row_offset = extract_lane(
                            simd_tag,
                            growth_maximum_row_offsets,
                            growth_lane);
                        const int16_t growth_column_offset = extract_lane(
                            simd_tag,
                            growth_maximum_column_offsets,
                            growth_lane);
                        best_query_position = block_row_start + previous_block_size
                            + static_cast<size_t>(growth_column_offset);
                        best_reference_position = block_column_start
                            + static_cast<size_t>(growth_row_offset)
                            + growth_lane;
                    }
                    break;
                }
            }

            if (block_size < maximum_block_size) {
                checkpoint_row_start = block_row_start; checkpoint_column_start = block_column_start; checkpoint_score_offset = score_offset;
                for (size_t i = 0; i < block_size; i += lane_count) {
                    scratch.diagonal_column_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_column, i);
                    scratch.gap_column_checkpoint.copy_vector_from(simd_tag, scratch.gap_column, i);
                    scratch.diagonal_row_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_row, i);
                    scratch.gap_row_checkpoint.copy_vector_from(simd_tag, scratch.gap_row, i);
                }
                if constexpr (p_is_trace_enabled) { scratch.trace.save_checkpoint(); }
                should_grow_without_maximum = false;
            }
            best_score = maximum_score_offset;
            growth_delay_iterations = 0;
        }

        // --- X-drop check ---
        if constexpr (p_is_x_drop_enabled) {
            if (maximum_score_offset < best_score - x_drop_threshold) {
                x_drop_confirmation_count += 1;
                if (x_drop_confirmation_count >= k_x_drop_confirmation_count) {
                    core_stats.is_x_drop_terminated = true;
                    break;
                }
            } else {
                x_drop_confirmation_count = 0;
            }
        }

        // --- End check ---
        if (block_row_start + block_size > query.length()
            && block_column_start + block_size > reference.length()) {
            break;
        }

        // --- Forced direction ---
        if (block_column_start + block_size > reference.length()) {
            block_row_start += k_block_shift_step; direction = block_direction::down; continue;
        }
        if (block_row_start + block_size > query.length()) {
            block_column_start += k_block_shift_step; direction = block_direction::right; continue;
        }

        // --- Y-drop grow ---
        size_t next_block_size = block_size * 2;
        if (next_block_size <= maximum_block_size) {
            if (growth_delay_iterations > (block_size / k_block_shift_step) - 1 || should_grow_without_maximum) {
                previous_block_size = block_size;
                block_size = next_block_size;
                direction = block_direction::grow;
                if constexpr (p_should_collect_stats) {
                    core_stats.add(core_stats.grow_events);
                }
                block_row_start = checkpoint_row_start; block_column_start = checkpoint_column_start; score_offset = checkpoint_score_offset;
                for (size_t i = 0; i < previous_block_size; i += lane_count) {
                    scratch.diagonal_column.copy_vector_from(simd_tag, scratch.diagonal_column_checkpoint, i);
                    scratch.gap_column.copy_vector_from(simd_tag, scratch.gap_column_checkpoint, i);
                    scratch.diagonal_row.copy_vector_from(simd_tag, scratch.diagonal_row_checkpoint, i);
                    scratch.gap_row.copy_vector_from(simd_tag, scratch.gap_row_checkpoint, i);
                }
                if constexpr (p_is_trace_enabled) { scratch.trace.restore_checkpoint(); }
                growth_delay_iterations = 0;
                continue;
            }
        }

        // --- Shrink ---
        if constexpr (k_block_shrink_enabled) {
            if (block_size > minimum_block_size && growth_delay_iterations == 0) {
                int16_t shrink_border_maximum = std::max(
                    block_suffix_maximum(simd_tag, scratch.diagonal_row.data(), block_size),
                    block_suffix_maximum(simd_tag, scratch.diagonal_column.data(), block_size));
                if (shrink_border_maximum >= current_maximum_score) {
                    previous_direction = block_direction::grow; // prevent diagonal_corner use
                    block_size /= 2;
                    if constexpr (p_should_collect_stats) {
                        core_stats.add(core_stats.shrink_events);
                    }
                    for (size_t i = 0; i < block_size; i += lane_count) {
                        scratch.diagonal_column.copy_vector(simd_tag, i, i + block_size);
                        scratch.gap_column.copy_vector(simd_tag, i, i + block_size);
                        scratch.diagonal_row.copy_vector(simd_tag, i, i + block_size);
                        scratch.gap_row.copy_vector(simd_tag, i, i + block_size);
                    }
                    block_row_start += block_size; block_column_start += block_size;
                    checkpoint_row_start = block_row_start; checkpoint_column_start = block_column_start; checkpoint_score_offset = score_offset;
                    for (size_t i = 0; i < block_size; i += lane_count) {
                        scratch.diagonal_column_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_column, i);
                        scratch.gap_column_checkpoint.copy_vector_from(simd_tag, scratch.gap_column, i);
                        scratch.diagonal_row_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_row, i);
                        scratch.gap_row_checkpoint.copy_vector_from(simd_tag, scratch.gap_row, i);
                    }
                    right_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_column.data());
                    down_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_row.data());
                    if constexpr (p_is_trace_enabled) { scratch.trace.save_checkpoint(); }
                    growth_delay_iterations = 0;
                }
            }
        }

        // --- Direction decision ---
        if (down_border_maximum > right_border_maximum) {
            block_row_start += k_block_shift_step; direction = block_direction::down;
        } else {
            block_column_start += k_block_shift_step; direction = block_direction::right;
        }
    } // main loop

    // --- Extract result ---
    if constexpr (p_is_x_drop_enabled) {
        output_score = best_score;
        output_query_position = best_query_position;
        output_reference_position = best_reference_position;
    } else {
        int16_t final_cell = k_block_score_bias;
        if (direction == block_direction::right || direction == block_direction::grow) {
            if (block_row_start > query.length()) {
                abort_block_aligner_invariant(
                    "global result query origin exceeded sequence length");
            }
            const size_t index = query.length() - block_row_start;
            if (index >= block_size) {
                abort_block_aligner_invariant(
                    "global result query border index exceeded block");
            }
            final_cell = scratch.diagonal_column.at(index);
        } else {
            if (block_column_start > reference.length()) {
                abort_block_aligner_invariant(
                    "global result reference origin exceeded sequence length");
            }
            const size_t index = reference.length() - block_column_start;
            if (index >= block_size) {
                abort_block_aligner_invariant(
                    "global result reference border index exceeded block");
            }
            final_cell = scratch.diagonal_row.at(index);
        }
        output_score = score_offset + static_cast<int32_t>(final_cell)
                       - static_cast<int32_t>(k_block_score_bias);
        output_query_position = query.length();
        output_reference_position = reference.length();
    }
}

}  // namespace target
}  // namespace bioseq::simd
