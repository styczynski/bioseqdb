/**
 * @file block_aligner_kernel.cppm
 * @brief Highway SIMD implementation of adaptive block alignment.
 *
 * Adaptive-block Smith-Waterman-Gotoh with single-affine gap penalties.
 * Maintains a sliding/growing/shrinking square block instead of computing
 * the full O(N*M) DP score_matrix.
 *
 * The result uses the shared ksw_alignment_result representation while the
 * recurrence, termination, traceback, and parameter contract retain native
 * adaptive-block semantics.
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

export module bioseq.highway.sources.block_aligner;

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

    /// Return the byte at a one-based padded index; index zero is the sentinel.
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
/// packed direction bits require initialization; DP writes the other arrays
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

// =============================================================================
// Traceback — CIGAR generation from recorded trace data
// =============================================================================

/** Packed CIGAR and optional edit count produced by traceback. */
struct block_traceback_result {
    std::vector<std::uint32_t> cigar;  ///< Packed KSW2-compatible operations.
    std::uint64_t edit_distance = 0;  ///< Exact unit-cost edit distance.
};

/**
 * Account for the exact unit-cost edit represented by one traceback step.
 *
 * Traceback coordinates are one-based sequence positions because index zero
 * in each block_padded_bytes is the DP sentinel. Reversed left extensions need
 * no special handling: both padded inputs and the trace use the same reversed
 * orientation, and edit distance is invariant when both strings are reversed.
 */
template <bool p_should_compute_edit_distance>
HWY_INLINE void record_block_traceback_edit(
    block_traceback_result& result,
    block_cigar_operation operation,
    size_t query_position,
    size_t reference_position,
    const block_padded_bytes& query,
    const block_padded_bytes& reference)
{
    if constexpr (!p_should_compute_edit_distance) {
        return;
    }
    switch (operation) {
        case block_cigar_operation::match:
            if (query_position == 0 || query_position > query.length()
                || reference_position == 0
                || reference_position > reference.length()) {
                abort_block_aligner_invariant(
                    "traceback diagonal edit coordinate exceeded input");
            }
            result.edit_distance +=
                query.byte_at(query_position) != reference.byte_at(reference_position);
            break;
        case block_cigar_operation::equal:
            break;
        case block_cigar_operation::mismatch:
        case block_cigar_operation::insertion:
        case block_cigar_operation::deletion:
            ++result.edit_distance;
            break;
        case block_cigar_operation::sentinel:
            abort_block_aligner_invariant(
                "traceback emitted a sentinel operation");
    }
}

/**
 * Materialize the packed CIGAR and, when requested, derive exact edit
 * accounting during the same traceback walk over resident padded ranks.
 */
template <bool p_should_compute_edit_distance>
[[nodiscard]] block_traceback_result build_block_cigar(
    const block_trace& trace,
    size_t query_end_position,
    size_t reference_end_position,
    const block_padded_bytes& query,
    const block_padded_bytes& reference)
{
    block_traceback_result result;
    auto& cigar = result.cigar;
    cigar.clear();
    if (query_end_position > (std::numeric_limits<size_t>::max)() - reference_end_position) {
        abort_block_aligner_invariant("traceback CIGAR capacity overflow");
    }
    // Every traceback step consumes at least one symbol, so this is an upper
    // bound on the number of packed runs.
    cigar.reserve(query_end_position + reference_end_position);

    // All diagonal trace variants project to the packed match opcode. Adjacent
    // operations are coalesced before materialization.
    block_cigar_operation pending_operation = block_cigar_operation::sentinel;
    size_t pending_length = 0;
    const auto flush_pending_operation = [&cigar, &pending_operation, &pending_length]() {
        if (pending_length == 0) return;

        uint32_t opcode = cigar_match_opcode;
        switch (pending_operation) {
            case block_cigar_operation::match:
            case block_cigar_operation::equal:
            case block_cigar_operation::mismatch: opcode = cigar_match_opcode; break;
            case block_cigar_operation::insertion: opcode = cigar_insertion_opcode; break;
            case block_cigar_operation::deletion: opcode = cigar_deletion_opcode; break;
            case block_cigar_operation::sentinel: return;
        }
        push_packed_cigar(
            cigar, opcode, static_cast<uint32_t>(pending_length));
    };
    const auto append_operation = [&flush_pending_operation, &pending_operation, &pending_length](
            block_cigar_operation operation) {
        if (operation == pending_operation) {
            ++pending_length;
            return;
        }
        flush_pending_operation();
        pending_operation = operation;
        pending_length = 1;
    };

    const size_t lane_count = hn::Lanes(block_aligner_simd_tag{});

    size_t block_count = trace.block_count();
    size_t trace_word_count = trace.trace_word_count();
    auto table = block_trace_table::diagonal;

    size_t query_position = query_end_position, reference_position = reference_end_position;

    while (query_position > 0 || reference_position > 0) {
        // Find the block containing the current traceback coordinate.
        size_t block_row_start, block_column_start, block_height, block_width;
        bool is_right_block;
        for (;;) {
            if (block_count == 0) {
                abort_block_aligner_invariant(
                    "traceback exhausted blocks before reaching origin");
            }
            block_count -= 1;
            block_row_start      = trace.block_origins()[block_count * 2];
            block_column_start      = trace.block_origins()[block_count * 2 + 1];
            block_height = trace.block_extents()[block_count * 2];
            block_width  = trace.block_extents()[block_count * 2 + 1];
            const size_t block_trace_words = block_width * block_height / lane_count;
            if (block_trace_words > trace_word_count) {
                abort_block_aligner_invariant("traceback trace index underflow");
            }
            trace_word_count -= block_trace_words;

            if (query_position >= block_row_start && reference_position >= block_column_start) {
                const size_t direction_word_index = block_count / 64;
                const size_t direction_bit_index  = block_count % 64;
                if (direction_word_index >= trace.block_directions().size()) {
                    abort_block_aligner_invariant(
                        "traceback direction index exceeded capacity");
                }
                is_right_block = (trace.block_directions()[direction_word_index] >> direction_bit_index) & 1;
                break;
            }
        }

        const auto& trace_lookup = k_block_trace_lookup[is_right_block ? 1 : 0];

        if (is_right_block) {
            // Right block: vectors vertical (height/lane_count per column)
            while (query_position >= block_row_start && reference_position >= block_column_start && (query_position > 0 || reference_position > 0)) {
                size_t row_offset = query_position - block_row_start;
                size_t column_offset = reference_position - block_column_start;
                size_t index = trace_word_count + row_offset / lane_count + column_offset * (block_height / lane_count);

                if (index >= trace.trace_word_count()
                    || index >= trace.primary_trace_words().size()
                    || index >= trace.secondary_trace_words().size()) {
                    abort_block_aligner_invariant(
                        "traceback cell index exceeded written trace");
                }

                uint32_t primary_trace_code  = (trace.primary_trace_words()[index]  >> ((row_offset % lane_count) * 2)) & 0b11;
                uint32_t secondary_trace_code = (trace.secondary_trace_words()[index] >> ((row_offset % lane_count) * 2)) & 0b11;

                size_t lookup_index = (primary_trace_code << 4) | (secondary_trace_code << 2)
                               | static_cast<size_t>(table);
                const auto& entry = trace_lookup[lookup_index];

                record_block_traceback_edit<p_should_compute_edit_distance>(
                    result, entry.operation, query_position, reference_position, query, reference);
                query_position -= entry.query_step;
                reference_position -= entry.reference_step;
                table = entry.next_table;
                append_operation(entry.operation);
            }
        } else {
            // Down block: vectors horizontal (width/lane_count per row)
            while (query_position >= block_row_start && reference_position >= block_column_start && (query_position > 0 || reference_position > 0)) {
                size_t row_offset = query_position - block_row_start;
                size_t column_offset = reference_position - block_column_start;
                size_t index = trace_word_count + column_offset / lane_count + row_offset * (block_width / lane_count);

                if (index >= trace.trace_word_count()
                    || index >= trace.primary_trace_words().size()
                    || index >= trace.secondary_trace_words().size()) {
                    abort_block_aligner_invariant(
                        "traceback cell index exceeded written trace");
                }

                uint32_t primary_trace_code  = (trace.primary_trace_words()[index]  >> ((column_offset % lane_count) * 2)) & 0b11;
                uint32_t secondary_trace_code = (trace.secondary_trace_words()[index] >> ((column_offset % lane_count) * 2)) & 0b11;

                size_t lookup_index = (primary_trace_code << 4) | (secondary_trace_code << 2)
                               | static_cast<size_t>(table);
                const auto& entry = trace_lookup[lookup_index];

                record_block_traceback_edit<p_should_compute_edit_distance>(
                    result, entry.operation, query_position, reference_position, query, reference);
                query_position -= entry.query_step;
                reference_position -= entry.reference_step;
                table = entry.next_table;
                append_operation(entry.operation);
            }
        }
    }
    flush_pending_operation();
    std::reverse(cigar.begin(), cigar.end());
    return result;
}

/** Return whether the score matrix has the generated diagonal layout. */
template<sequence_alphabet p_alphabet,
         size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
[[nodiscard]] bool has_generated_diagonal_score_layout(
        const scoring_matrix<p_alphabet>& scoring_matrix,
        std::int8_t& match_score,
        std::int8_t& mismatch_score) noexcept {
    // The final alphabet rank is the padding sentinel. All biological diagonal
    // entries and all biological off-diagonal entries must be uniform.
    if constexpr (p_alphabet_dimension < 3) return false;
    constexpr size_t biological_ranks = p_alphabet_dimension - 1;
    match_score = scoring_matrix.at(0, 0);
    mismatch_score = scoring_matrix.at(0, 1);
    const std::int8_t ambiguity_score = scoring_matrix.at(
        0, static_cast<int>(biological_ranks));
    if (match_score < 0) return false;
    for (size_t row = 0; row < p_alphabet_dimension; ++row) {
        for (size_t col = 0; col < p_alphabet_dimension; ++col) {
            const std::int8_t expected =
                row == biological_ranks || col == biological_ranks
                    ? ambiguity_score
                    : row == col ? match_score : mismatch_score;
            const std::int8_t observed = scoring_matrix.at(
                static_cast<int>(row), static_cast<int>(col));
            if (observed != expected || observed > match_score) return false;
        }
    }
    return true;
}

/** Return a low-bit mask for up to 32 packed two-bit DNA4 symbols. */
[[nodiscard]] constexpr std::uint64_t low_dna4_symbol_mask(
        size_t symbol_count) noexcept {
    return symbol_count == 32u
        ? (std::numeric_limits<std::uint64_t>::max)()
        : (std::uint64_t{1} << (symbol_count * 2u)) - 1u;
}

/// Load up to 32 LSB-first DNA4 ranks from an arbitrary base coordinate.
/// The second storage unit is touched only when the requested window crosses
/// the first unit boundary, so the final partial unit is never over-read.
[[nodiscard]] HWY_INLINE std::uint64_t load_forward_dna4_window(
        const std::uint64_t* packed_units,
        size_t base_position,
        size_t symbol_count) noexcept {
    constexpr size_t symbols_per_unit = 32u;
    const size_t unit_index = base_position / symbols_per_unit;
    const size_t symbol_offset = base_position % symbols_per_unit;
    const size_t bit_offset = symbol_offset * 2u;
    std::uint64_t packed = packed_units[unit_index] >> bit_offset;
    if (symbol_offset != 0u
        && symbol_offset + symbol_count > symbols_per_unit) {
        packed |= packed_units[unit_index + 1u] << (64u - bit_offset);
    }
    return packed & low_dna4_symbol_mask(symbol_count);
}

/// Reverse the order of 32 two-bit groups while retaining the bit order
/// within each rank.  DNA4 complement is a subsequent XOR with 0b11/rank.
[[nodiscard]] constexpr std::uint64_t reverse_dna4_groups(
        std::uint64_t packed) noexcept {
    packed = ((packed & 0x0303030303030303ULL) << 6u)
        | ((packed & 0x0c0c0c0c0c0c0c0cULL) << 2u)
        | ((packed & 0x3030303030303030ULL) >> 2u)
        | ((packed & 0xc0c0c0c0c0c0c0c0ULL) >> 6u);
    return std::byteswap(packed);
}

/** Load one forward or reverse-complement packed DNA4 window. */
template<bool p_is_reversed>
[[nodiscard]] HWY_INLINE std::uint64_t load_oriented_dna4_window(
        const std::uint64_t* packed_units,
        size_t total_symbols,
        size_t logical_start,
        size_t symbol_count) noexcept {
    if constexpr (!p_is_reversed) {
        return load_forward_dna4_window(
            packed_units, logical_start, symbol_count);
    } else {
        const size_t source_end = total_symbols - logical_start;
        const size_t source_begin = source_end - symbol_count;
        const std::uint64_t forward_window = load_forward_dna4_window(
            packed_units, source_begin, symbol_count);
        const size_t inactive_symbols = 32u - symbol_count;
        const std::uint64_t reversed_window = reverse_dna4_groups(forward_window)
            >> (inactive_symbols * 2u);
        return reversed_window ^ low_dna4_symbol_mask(symbol_count);
    }
}

/// Prove that an equal-length global alignment's coordinate diagonal is its
/// unique optimum.  This is deliberately a byte-and-score certificate, not a
/// seed/hash shortcut: the scan consumes the exact encoded bytes that the DP
/// would consume.
///
/// Let `sequence_length` be the common length, `diagonal_score` the coordinate
/// diagonal score, `maximum_substitution_score` the largest matrix entry, and
/// q/e the positive affine penalties. Any different path contains at least one
/// insertion and one deletion, which yields a strict alternative-score bound.
///
/// A diagonal score above that bound proves both optimality and CIGAR
/// uniqueness; equality is insufficient because traceback ties are observable.
template<
    bool p_is_reversed,
    sequence_alphabet p_alphabet,
    size_t p_alphabet_dimension>
[[nodiscard]] HWY_INLINE block_diagonal_certificate
try_global_diagonal_certificate(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const block_aligner_parameters& parameters) {
    block_diagonal_certificate result;
    const bool are_coordinates_ordered = query_end >= query_start
        && target_end >= target_start;
    const size_t sequence_length = are_coordinates_ordered ? query_end - query_start : 0u;
    const size_t target_length = are_coordinates_ordered
        ? target_end - target_start : 0u;
    if (parameters.mode != block_alignment_mode::global
        || parameters.stage != block_alignment_stage::gap_extension
        || parameters.input_order != block_input_order::forward
        || parameters.gap_open <= 0 || parameters.gap_extend <= 0
        || !are_coordinates_ordered || sequence_length == 0 || target_length != sequence_length
        || query_end > query_seq.bp_size()
        || target_end > target_seq.bp_size()
        || sequence_length > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return result;
    }
    constexpr size_t packed_cigar_limit = static_cast<size_t>(
        std::numeric_limits<std::uint32_t>::max() >> 4u);
    if (parameters.is_trace_enabled && sequence_length > packed_cigar_limit) return result;

    // The scan classifies equal and unequal biological ranks, so admit only a
    // matrix layout that this classification scores exactly.
    if constexpr (p_alphabet_dimension < 3) return result;
    constexpr size_t biological_ranks = p_alphabet_dimension - 1;
    std::int8_t match_score = 0;
    std::int8_t mismatch_score = 0;
    if (!has_generated_diagonal_score_layout<p_alphabet, p_alphabet_dimension>(
            scoring_matrix, match_score, mismatch_score)) {
        return result;
    }

    // Encode bounded chunks in the same orientation as the DP. Certified calls
    // avoid arena-backed padded, scratch, and trace storage.
    constexpr size_t scan_chunk_symbols = 1024u;
    // Every inspected byte is written by the encoder. Only the inactive tail
    // of a masked hardware load is initialized below.
    alignas(64) std::array<std::uint8_t, scan_chunk_symbols> query_chunk;
    alignas(64) std::array<std::uint8_t, scan_chunk_symbols> target_chunk;
    const hn::Rebind<uint8_t, block_aligner_simd_tag> byte_tag;
    const size_t lane_count = hn::Lanes(byte_tag);
    const auto rank_limit = hn::Set(
        byte_tag, static_cast<uint8_t>(biological_ranks));
    std::uint64_t equal_symbols = 0;
    for (size_t chunk_offset = 0; chunk_offset < sequence_length;
         chunk_offset += scan_chunk_symbols) {
        const size_t chunk_size = std::min(
            scan_chunk_symbols, sequence_length - chunk_offset);
        query_seq.template copy_ranks<p_is_reversed, false>(
            std::span<std::uint8_t>{query_chunk.data(), chunk_size},
            query_start + chunk_offset,
            query_start + chunk_offset + chunk_size);
        target_seq.template copy_ranks<false, false>(
            std::span<std::uint8_t>{target_chunk.data(), chunk_size},
            target_start + chunk_offset,
            target_start + chunk_offset + chunk_size);
        // Highway's masked load is permitted to issue a full hardware load
        // (notably on NEON). Initialise only the inactive bytes of the one
        // partial vector so that load never observes indeterminate storage.
        // Full chunks are lane-aligned for every admitted lane shape.
        const size_t tail_lanes = chunk_size % lane_count;
        if (tail_lanes != 0) {
            const size_t tail_padding = lane_count - tail_lanes;
            std::memset(
                query_chunk.data() + chunk_size, 0, tail_padding);
            std::memset(
                target_chunk.data() + chunk_size, 0, tail_padding);
        }
        for (size_t offset = 0; offset < chunk_size; offset += lane_count) {
            const size_t active_lanes = std::min(
                lane_count, chunk_size - offset);
            const auto active = hn::FirstN(byte_tag, active_lanes);
            const auto query_bytes = hn::MaskedLoad(
                active, byte_tag, query_chunk.data() + offset);
            const auto reference_bytes = hn::MaskedLoad(
                active, byte_tag, target_chunk.data() + offset);
            const auto valid_ranks = hn::And(
                hn::Lt(query_bytes, rank_limit),
                hn::Lt(reference_bytes, rank_limit));
            if (hn::CountTrue(byte_tag, hn::And(active, valid_ranks))
                != active_lanes) {
                return block_diagonal_certificate{};
            }
            equal_symbols += hn::CountTrue(
                byte_tag, hn::And(active, hn::Eq(
                    query_bytes, reference_bytes)));
            ++result.vector_batches;
        }
    }

    const std::uint64_t unequal_symbols = static_cast<std::uint64_t>(sequence_length)
        - equal_symbols;
    const int64_t diagonal_score =
        static_cast<int64_t>(equal_symbols) * match_score
        + static_cast<int64_t>(unequal_symbols) * mismatch_score;
    const int64_t alternative_upper_bound =
        static_cast<int64_t>(sequence_length - 1) * match_score
        - 2 * (static_cast<int64_t>(parameters.gap_open)
            + static_cast<int64_t>(parameters.gap_extend));
    if (diagonal_score <= alternative_upper_bound
        || diagonal_score < std::numeric_limits<int32_t>::min()
        || diagonal_score > std::numeric_limits<int32_t>::max()) {
        return block_diagonal_certificate{};
    }

    result.is_certified = true;
    result.score = static_cast<int32_t>(diagonal_score);
    result.edit_distance = unequal_symbols;
    result.symbols = sequence_length;
    return result;
}

/// Attempt the resident packed DNA4/u64 certificate without materializing
/// byte ranks. The three-way result is important to the caller:
///
///  * not_eligible: no packed content scan ran; use the byte-rank path.
///  * rejected: the exact packed content scan ran but could not prove the
///    diagonal; enter DP without repeating the byte certificate scan.
///  * certified: caller may commit the exact score/endpoints/M run and record
///    one logical adaptive-block execution.
template<
    bool p_is_reversed,
    sequence_alphabet p_alphabet>
[[nodiscard]] HWY_INLINE block_packed_certificate_attempt
try_packed_global_gap_certificate(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const block_global_gap_certificate_plan& plan) noexcept {
    block_packed_certificate_attempt attempt;
    if constexpr (!std::same_as<p_alphabet, seq::dna4>) {
        return attempt;
    } else {
        const bool are_coordinates_ordered = query_end >= query_start
            && target_end >= target_start;
        const size_t query_length = are_coordinates_ordered
            ? query_end - query_start : 0u;
        const size_t target_length = are_coordinates_ordered
            ? target_end - target_start : 0u;
        constexpr size_t packed_cigar_limit = static_cast<size_t>(
            std::numeric_limits<std::uint32_t>::max() >> 4u);
        if (!plan.is_contract_supported
            || !plan.is_generated_matrix_supported
            || plan.parameters.mode != block_alignment_mode::global
            || plan.parameters.stage != block_alignment_stage::gap_extension
            || plan.parameters.input_order != block_input_order::forward
            || !are_coordinates_ordered || query_length == 0u
            || target_length != query_length
            || query_end > query_seq.bp_size()
            || target_end > target_seq.bp_size()
            || query_length > static_cast<size_t>(
                std::numeric_limits<std::int32_t>::max())
            || (plan.parameters.is_trace_enabled && query_length > packed_cigar_limit)) {
            return attempt;
        }

        // Borrow the complete logical facade descriptors.  Region-only probes
        // lose the logical totals required by reverse orientation and expose
        // nonzero leading offsets for ordinary unaligned slices.  The packed
        // loader already handles arbitrary symbol coordinates, so retain each
        // descriptor's physical leading offset explicitly.
        const auto query_region = query_seq.try_resident_packed(
            0u, query_seq.bp_size(), 8u);
        const auto target_region = target_seq.try_resident_packed(
            0u, target_seq.bp_size(), 8u);
        if (!query_region.has_value() || !target_region.has_value()
            || query_seq.bp_size()
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<size_t>::max)())
            || target_seq.bp_size()
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<size_t>::max)())) {
            return attempt;
        }
        const auto* query_units =
            static_cast<const std::uint64_t*>(query_region->units);
        const auto* target_units =
            static_cast<const std::uint64_t*>(target_region->units);
        const size_t query_logical_total =
            static_cast<size_t>(query_seq.bp_size());
        const size_t target_logical_total =
            static_cast<size_t>(target_seq.bp_size());
        const size_t query_leading = query_region->leading_symbol_offset;
        const size_t target_leading = target_region->leading_symbol_offset;
        if (query_logical_total
                > (std::numeric_limits<size_t>::max)() - query_leading
            || target_logical_total
                > (std::numeric_limits<size_t>::max)() - target_leading) {
            return attempt;
        }
        const size_t query_physical_total =
            query_leading + query_logical_total;

        // Extraction handles arbitrary base offsets one word at a time.  The
        // staging arrays are completely initialized before each masked load;
        // Highway then collapses and popcounts two-bit mismatches across the
        // native packed-word lane_count.
        const hn::Repartition<std::uint64_t, block_aligner_simd_tag> packed_word_tag;
        const size_t lane_count = hn::Lanes(packed_word_tag);
        constexpr size_t max_packed_lanes = 4u;
        alignas(64) std::array<std::uint64_t, max_packed_lanes> query_words{};
        alignas(64) std::array<std::uint64_t, max_packed_lanes> target_words{};
        const auto pair_low_bits = hn::Set(packed_word_tag, 0x5555555555555555ULL);
        const size_t word_count = divide_block_extent_round_up(query_length, 32u);
        std::uint64_t mismatch_symbols = 0;
        for (size_t first_word = 0; first_word < word_count;
             first_word += lane_count) {
            const size_t active_lanes = std::min(lane_count,
                word_count - first_word);
            for (size_t lane = 0; lane < active_lanes; ++lane) {
                const size_t symbol_offset = (first_word + lane) * 32u;
                const size_t symbol_count = std::min(
                    size_t{32}, query_length - symbol_offset);
                const size_t query_logical_start =
                    query_start + symbol_offset;
                query_words[lane] = load_oriented_dna4_window<p_is_reversed>(
                    query_units,
                    query_physical_total,
                    p_is_reversed
                        ? query_logical_start
                        : query_leading + query_logical_start,
                    symbol_count);
                target_words[lane] = load_oriented_dna4_window<false>(
                    target_units, target_leading + target_logical_total,
                    target_leading + target_start + symbol_offset,
                    symbol_count);
            }
            const auto active = hn::FirstN(packed_word_tag, active_lanes);
            const auto query_packed = hn::MaskedLoad(
                active, packed_word_tag, query_words.data());
            const auto target_packed = hn::MaskedLoad(
                active, packed_word_tag, target_words.data());
            const auto different_bits = hn::Xor(query_packed, target_packed);
            const auto different_symbols = hn::And(
                hn::Or(different_bits, hn::ShiftRight<1>(different_bits)),
                pair_low_bits);
            const auto per_lane_mismatches = hn::PopulationCount(
                different_symbols);
            mismatch_symbols += hn::ReduceSum(
                packed_word_tag, hn::IfThenElseZero(active, per_lane_mismatches));
            ++attempt.certificate.vector_batches;
        }

        attempt.decision = block_packed_certificate_decision::rejected;
        const std::uint64_t equal_symbols =
            static_cast<std::uint64_t>(query_length) - mismatch_symbols;
        const std::int64_t diagonal_score =
            static_cast<std::int64_t>(equal_symbols) * plan.match_score
            + static_cast<std::int64_t>(mismatch_symbols)
                * plan.mismatch_score;
        const std::int64_t alternative_upper_bound =
            static_cast<std::int64_t>(query_length - 1u) * plan.match_score
            - 2 * (static_cast<std::int64_t>(plan.parameters.gap_open)
                + static_cast<std::int64_t>(plan.parameters.gap_extend));
        if (diagonal_score <= alternative_upper_bound
            || diagonal_score < std::numeric_limits<std::int32_t>::min()
            || diagonal_score > std::numeric_limits<std::int32_t>::max()) {
            return attempt;
        }

        attempt.decision = block_packed_certificate_decision::certified;
        attempt.certificate.is_certified = true;
        attempt.certificate.score = static_cast<std::int32_t>(diagonal_score);
        attempt.certificate.edit_distance = mismatch_symbols;
        attempt.certificate.symbols = query_length;
        return attempt;
    }
}

/** Record an admitted call before any zero-work or SIMD fast path executes. */
inline void record_block_aligner_eligibility(
        block_aligner_execution_stats& stats,
        const block_aligner_parameters& parameters,
        std::size_t query_length,
        std::size_t target_length,
        std::uint64_t configuration_identity) noexcept {
    switch (parameters.stage) {
    case block_alignment_stage::left_extension: stats.add(stats.left_eligible); break;
    case block_alignment_stage::gap_extension: stats.add(stats.gap_eligible); break;
    case block_alignment_stage::right_extension: stats.add(stats.right_eligible); break;
    }
    if (parameters.mode == block_alignment_mode::global) stats.add(stats.global_calls);
    else stats.add(stats.native_x_drop_calls);
    if (parameters.is_trace_enabled) stats.add(stats.trace_calls);
    if (parameters.input_order == block_input_order::reversed) {
        stats.add(stats.reversed_input_calls);
        if (query_length != 0 && target_length != 0) {
            stats.add(stats.expected_reversed_compact_symbols, query_length);
            stats.add(stats.expected_reversed_compact_symbols, target_length);
        }
    } else {
        stats.add(stats.forward_input_calls);
    }
    stats.add(stats.configuration_identity_calls);
    stats.configuration_identity_xor ^= configuration_identity;
    stats.configuration_identity_sum += configuration_identity;
}

/** Record the executed path and exact SIMD work for one admitted call. */
inline void record_block_aligner_execution(
        block_aligner_execution_stats& stats,
        const block_aligner_parameters& parameters,
        std::size_t lane_count,
        bool is_zero_work,
        const block_core_stats& core_stats,
        std::uint64_t configuration_identity) noexcept {
    switch (parameters.stage) {
    case block_alignment_stage::left_extension: stats.add(stats.left_executed); break;
    case block_alignment_stage::gap_extension: stats.add(stats.gap_executed); break;
    case block_alignment_stage::right_extension: stats.add(stats.right_executed); break;
    }
    if (parameters.mode == block_alignment_mode::global) {
        stats.add(stats.executed_global_calls);
    } else {
        stats.add(stats.executed_native_x_drop_calls);
    }
    if (parameters.is_trace_enabled) stats.add(stats.executed_trace_calls);
    if (parameters.input_order == block_input_order::reversed) {
        stats.add(stats.executed_reversed_input_calls);
    } else {
        stats.add(stats.executed_forward_input_calls);
    }
    if (lane_count == 8) stats.add(stats.simd_eight_lane_calls);
    else if (lane_count == 16) stats.add(stats.simd_sixteen_lane_calls);
    else stats.is_saturated = true;
    if (is_zero_work) {
        stats.add(stats.zero_work_calls);
    } else {
        stats.add(stats.nonempty_calls);
        const bool was_simd_dp_executed = core_stats.vector_batches != 0
            && core_stats.cells != 0;
        const bool was_simd_certificate_executed = core_stats.is_diagonal_certified
            && core_stats.diagonal_certificate_vector_batches != 0
            && core_stats.diagonal_certificate_symbols != 0;
        if ((was_simd_dp_executed || was_simd_certificate_executed)
            && !core_stats.is_saturated) {
            stats.add(stats.nonempty_calls_with_simd);
        }
    }
    stats.add(stats.simd_dp_vector_batches, core_stats.vector_batches);
    stats.add(stats.simd_dp_cells, core_stats.cells);
    if (core_stats.is_diagonal_certified) {
        stats.add(stats.simd_diagonal_certificate_calls);
    }
    stats.add(stats.simd_diagonal_certificate_vector_batches,
        core_stats.diagonal_certificate_vector_batches);
    stats.add(stats.simd_diagonal_certificate_symbols,
        core_stats.diagonal_certificate_symbols);
    stats.add(stats.adaptive_grow_events, core_stats.grow_events);
    stats.add(stats.adaptive_shrink_events, core_stats.shrink_events);
    if (core_stats.is_x_drop_terminated) {
        stats.add(stats.native_x_drop_terminations);
    }
    stats.is_saturated = stats.is_saturated || core_stats.is_saturated;
    stats.add(stats.executed_configuration_identity_calls);
    stats.executed_configuration_identity_xor ^= configuration_identity;
    stats.executed_configuration_identity_sum += configuration_identity;
}

/** Abort a forbidden attempt to execute biological work with scalar DP. */
[[noreturn]] inline void abort_forbidden_scalar_biological_fallback(
        block_aligner_execution_stats& stats) {
    stats.add(stats.forbidden_scalar_biological_fallbacks);
    bioseq_log<logging_level::error>(
        "Block Aligner attempted forbidden scalar biological fallback");
    std::abort();
}

// =============================================================================
// Fixed-target implementation
// =============================================================================

/** Fixed-target adaptive-block implementation used by generated callables. */
class highway_block_aligner {
public:
    /** Precompute invariant contract and matrix checks for packed gaps. */
    template<
        sequence_alphabet p_alphabet,
        size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
    [[nodiscard]] static block_global_gap_certificate_plan
    prepare_global_gap_certificate(
            const scoring_matrix<p_alphabet>& scoring_matrix,
            const block_aligner_parameters& parameters) noexcept {
        block_global_gap_certificate_plan plan{
            .parameters = parameters,
            .scoring_identity = scoring_matrix.block_semantic_identity(),
        };
        const size_t lane_count = hn::Lanes(block_aligner_simd_tag{});
        plan.simd_lane_count = static_cast<std::uint8_t>(lane_count);
        plan.is_contract_supported = scoring_matrix.is_block_layout_valid()
            && validate_block_aligner_contract(parameters, lane_count)
                == block_aligner_contract_error::none;
        plan.is_generated_matrix_supported =
            has_generated_diagonal_score_layout<p_alphabet, p_alphabet_dimension>(
                scoring_matrix, plan.match_score, plan.mismatch_score);
        return plan;
    }

    /** Attempt packed resident certification for one oriented gap. */
    template<
        bool p_is_reversed,
        sequence_alphabet p_alphabet>
    [[nodiscard]] static HWY_INLINE block_packed_certificate_attempt
    try_packed_global_gap(
            bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
            bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
            size_t query_start,
            size_t query_end,
            size_t target_start,
            size_t target_end,
            const block_global_gap_certificate_plan& plan) noexcept {
        return try_packed_global_gap_certificate<p_is_reversed>(
            query_seq, target_seq,
            query_start, query_end, target_start, target_end, plan);
    }

    /// Run block aligner on a sub-region of the given sequences.
    /// Returns ksw_alignment_result as the pipeline's shared result storage ABI.
    ///
    /// Gap convention: gap_open/gap_extend are the positive minimap q/e
    /// encoding only; this does not request KSW recurrence semantics. They are
    /// converted to the reference's negative penalties:
    ///   internal_open   = -(gap_open + gap_extend)
    ///   internal_extend = -gap_extend
    template<
        bool p_is_reversed,
        bool p_is_diagonal_certificate_enabled = true,
        sequence_alphabet p_alphabet,
        size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
    [[nodiscard]] static ksw_alignment_result align(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const block_aligner_parameters& parameters,
        block_aligner_execution_stats* execution_stats = nullptr,
        block_aligner_construction_stats* construction_stats = nullptr)
    {
        static_assert(
            p_alphabet_dimension
                == seqan3::alphabet_size<p_alphabet> + 1u,
            "Block Aligner scoring/padding alphabet dimension must match input");

        ksw_alignment_result result;

        const block_aligner_simd_tag simd_tag;
        const size_t lane_count = hn::Lanes(simd_tag);
        if (execution_stats != nullptr
            && !is_block_aligner_lane_shape_supported(lane_count)) {
            // The native path has no scalar biological implementation. Make
            // the forbidden boundary operational and fail before any work.
            abort_forbidden_scalar_biological_fallback(
                *execution_stats);
        }
        const auto contract_error = validate_block_aligner_contract(parameters, lane_count);
        const bool is_scoring_matrix_valid = scoring_matrix.is_block_layout_valid();
        const bool are_coordinates_ordered = query_end >= query_start
            && target_end >= target_start;
        const size_t query_length = are_coordinates_ordered
            ? query_end - query_start : 0;
        const size_t target_length = are_coordinates_ordered
            ? target_end - target_start : 0;
        constexpr size_t max_result_coordinate = static_cast<size_t>(
            std::numeric_limits<int32_t>::max());
        constexpr size_t max_packed_cigar_run = static_cast<size_t>(
            std::numeric_limits<std::uint32_t>::max() >> 4u);
        const bool are_coordinates_valid = are_coordinates_ordered
            && query_end <= query_seq.bp_size()
            && target_end <= target_seq.bp_size()
            && query_length <= max_result_coordinate
            && target_length <= max_result_coordinate
            && (!parameters.is_trace_enabled
                || (query_length <= max_packed_cigar_run
                    && target_length <= max_packed_cigar_run));
        const auto effective_contract_error = !are_coordinates_valid
            ? block_aligner_contract_error::coordinates
            : !is_scoring_matrix_valid
                ? block_aligner_contract_error::scoring_matrix
                : contract_error;
        if (effective_contract_error != block_aligner_contract_error::none) {
            if (execution_stats != nullptr) {
                execution_stats->add(
                    execution_stats->unsupported_contract_rejections);
            }
            bioseq_log<logging_level::error>(
                "Block Aligner rejected contract (error={}, are_coordinates_valid={})",
                static_cast<unsigned>(effective_contract_error),
                are_coordinates_valid);
            std::abort();
        }
        const std::uint64_t configuration_identity =
            ::bioseq::simd::block_aligner_configuration_identity(
                parameters, scoring_matrix.block_semantic_identity(), p_is_reversed);
        if (execution_stats != nullptr) {
            record_block_aligner_eligibility(
                *execution_stats, parameters, query_length, target_length,
                configuration_identity);
        }

        if (query_length == 0 || target_length == 0) {
            if (parameters.mode == block_alignment_mode::x_drop) {
                result.score = 0;
                result.max = 0;
            } else {
                const size_t gap_len = std::max(query_length, target_length);
                result.score = gap_len == 0 ? 0 : -(
                    static_cast<int32_t>(parameters.gap_open)
                    + static_cast<int32_t>(parameters.gap_extend) * gap_len);
                result.max = result.score;
                result.max_q = static_cast<int32_t>(query_length) - 1;
                result.max_t = static_cast<int32_t>(target_length) - 1;
                if (parameters.is_trace_enabled && gap_len != 0) {
                    const std::uint32_t opcode = query_length == 0
                        ? cigar_deletion_opcode : cigar_insertion_opcode;
                    result.cigar.push_back(
                        (static_cast<std::uint32_t>(gap_len) << 4u) | opcode);
                }
                result.has_reached_end = true;
                result.mqe = result.score;
                result.mqe_t = result.max_t;
                result.mte = result.score;
                result.mte_q = result.max_q;
            }
            if (execution_stats != nullptr) {
                record_block_aligner_execution(
                    *execution_stats, parameters, lane_count, true, block_core_stats{},
                    configuration_identity);
            }
            if (parameters.should_compute_edit_distance) {
                result.edit_distance = std::max(query_length, target_length);
                result.is_edit_distance_valid = true;
            }
            return result;
        }

        block_core_stats core_stats;
        if constexpr (p_is_diagonal_certificate_enabled) {
            const block_diagonal_certificate certificate =
                try_global_diagonal_certificate<
                    p_is_reversed, p_alphabet, p_alphabet_dimension>(
                        query_seq, target_seq,
                        query_start, query_end, target_start, target_end,
                        scoring_matrix, parameters);
            if (certificate.is_certified) {
                result.score = certificate.score;
                result.max = certificate.score;
                result.max_q = static_cast<int32_t>(query_length) - 1;
                result.max_t = static_cast<int32_t>(target_length) - 1;
                result.mqe = certificate.score;
                result.mqe_t = result.max_t;
                result.mte = certificate.score;
                result.mte_q = result.max_q;
                result.has_reached_end = true;
                if (parameters.is_trace_enabled) {
                    result.cigar.push_back(
                        (static_cast<std::uint32_t>(query_length) << 4u)
                        | cigar_match_opcode);
                }
                if (parameters.should_compute_edit_distance) {
                    result.edit_distance = certificate.edit_distance;
                    result.is_edit_distance_valid = true;
                }
                core_stats.is_diagonal_certified = true;
                core_stats.diagonal_certificate_vector_batches =
                    certificate.vector_batches;
                core_stats.diagonal_certificate_symbols = certificate.symbols;
                if (execution_stats != nullptr) {
                    record_block_aligner_execution(
                        *execution_stats, parameters, lane_count, false, core_stats,
                        configuration_identity);
                }
                return result;
            }
        }

        // Arena-backed padded, border, scratch, and trace storage is scoped to
        // this alignment. The result owns its CIGAR independently.
        if (construction_stats != nullptr) {
            ++construction_stats->align_arena_scopes;
        }
        auto align_scope = context::current().push<context::arena::alignment>();

        const size_t minimum_block_size = std::max(parameters.minimum_block_size, lane_count);
        const size_t maximum_block_size = std::max(parameters.maximum_block_size, lane_count);

        // Convert positive q/e into the reference's negative gap costs.
        const int8_t negative_gap_open = static_cast<int8_t>(
            -(static_cast<int>(parameters.gap_open) + parameters.gap_extend));
        const int8_t negative_gap_extension = static_cast<int8_t>(-parameters.gap_extend);

        block_padded_bytes query_padded, target_padded;
        if (construction_stats != nullptr) {
            construction_stats->padded_sequence_allocations += 2u;
        }
        if (parameters.input_order == block_input_order::reversed) {
            // Emit reversed alignment orientation directly from compact
            // storage without a scalar whole-buffer reverse pass.
            query_padded.template encode_region<p_is_reversed, true, p_alphabet_dimension>(
                query_seq, query_start, query_end, maximum_block_size);
            target_padded.template encode_region<false, true, p_alphabet_dimension>(
                target_seq, target_start, target_end, maximum_block_size);
            if (execution_stats != nullptr) {
                execution_stats->add(
                    execution_stats->reversed_compact_symbols, query_length);
                execution_stats->add(
                    execution_stats->reversed_compact_symbols, target_length);
            }
        } else {
            // Encode the sub-region directly into its padded DP buffer.
            query_padded.template encode_region<p_is_reversed, false, p_alphabet_dimension>(
                query_seq, query_start, query_end, maximum_block_size);
            target_padded.template encode_region<false, false, p_alphabet_dimension>(
                target_seq, target_start, target_end, maximum_block_size);
        }

        const bool is_trace_enabled = parameters.is_trace_enabled;
        if (construction_stats != nullptr) {
            ++construction_stats->scratch_allocations;
            construction_stats->trace_allocations += is_trace_enabled ? 1u : 0u;
        }
        block_aligner_scratch scratch(query_length, target_length, maximum_block_size, is_trace_enabled);
        scratch.clear(simd_tag, query_length, target_length, maximum_block_size, is_trace_enabled);

        int32_t score = 0;
        size_t output_query_position = 0, output_reference_position = 0;

        const bool is_x_drop_enabled = parameters.mode == block_alignment_mode::x_drop;
        const auto run_core = [&]<
                bool p_is_trace_enabled,
                bool p_is_x_drop_enabled>(
                std::bool_constant<p_is_trace_enabled>,
                std::bool_constant<p_is_x_drop_enabled>) {
            constexpr bool k_should_collect_stats = true;
            const int32_t effective_x_drop_threshold = p_is_x_drop_enabled
                ? parameters.x_drop_threshold
                : 0;
            if (execution_stats != nullptr) {
                align_block_core<
                    p_is_trace_enabled,
                    p_is_x_drop_enabled,
                    k_should_collect_stats,
                    p_alphabet>(
                    simd_tag, score, output_query_position, output_reference_position, scratch,
                    scoring_matrix, negative_gap_open, negative_gap_extension,
                    query_padded, target_padded, minimum_block_size, maximum_block_size,
                    effective_x_drop_threshold, core_stats);
            } else {
                align_block_core<
                    p_is_trace_enabled,
                    p_is_x_drop_enabled,
                    !k_should_collect_stats,
                    p_alphabet>(
                    simd_tag, score, output_query_position, output_reference_position, scratch,
                    scoring_matrix, negative_gap_open, negative_gap_extension,
                    query_padded, target_padded, minimum_block_size, maximum_block_size,
                    effective_x_drop_threshold, core_stats);
            }
        };

        if (is_trace_enabled && is_x_drop_enabled) {
            run_core(std::true_type{}, std::true_type{});
        } else if (is_trace_enabled) {
            run_core(std::true_type{}, std::false_type{});
        } else if (is_x_drop_enabled) {
            run_core(std::false_type{}, std::true_type{});
        } else {
            run_core(std::false_type{}, std::false_type{});
        }

        if (is_trace_enabled) {
            block_traceback_result traceback_result =
                parameters.should_compute_edit_distance
                ? build_block_cigar<true>(
                    scratch.trace, output_query_position, output_reference_position,
                    query_padded, target_padded)
                : build_block_cigar<false>(
                    scratch.trace, output_query_position, output_reference_position,
                    query_padded, target_padded);
            result.cigar = std::move(traceback_result.cigar);
            if (parameters.input_order == block_input_order::reversed) {
                std::reverse(result.cigar.begin(), result.cigar.end());
            }
            if (parameters.should_compute_edit_distance) {
                result.edit_distance = traceback_result.edit_distance;
                result.is_edit_distance_valid = true;
            }
        }

        result.score = score;
        result.max = score;
        result.max_q = static_cast<int32_t>(output_query_position) - 1;
        result.max_t = static_cast<int32_t>(output_reference_position) - 1;
        result.is_z_dropped = core_stats.is_x_drop_terminated;

        if (output_query_position >= query_length) {
            result.mqe = score;
            result.mqe_t = static_cast<int32_t>(output_reference_position) - 1;
            // Query completion uses the exclusive coordinate returned by the
            // adaptive-block recurrence; the reference may stop earlier.
            result.has_reached_end = true;
        }
        if (output_reference_position >= target_length) {
            result.mte = score;
            result.mte_q = static_cast<int32_t>(output_query_position) - 1;
        }

        if (execution_stats != nullptr) {
            record_block_aligner_execution(
                *execution_stats, parameters, lane_count, false, core_stats,
                configuration_identity);
        }

        return result;
    }
};

/** Prepare invariant packed-gap certificate state for the active target. */
template<sequence_alphabet p_alphabet>
[[nodiscard]] block_global_gap_certificate_plan prepare_block_global_gap_certificate(
    const scoring_matrix<p_alphabet>& scoring_matrix,
    const block_aligner_parameters& parameters) noexcept
{
    return highway_block_aligner::prepare_global_gap_certificate<p_alphabet>(
        scoring_matrix, parameters);
}

/** Attempt packed certification for one gap in the requested orientation. */
template<sequence_alphabet p_alphabet>
[[nodiscard]] block_packed_certificate_attempt try_block_certified_gap(
    bool is_reversed,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const block_global_gap_certificate_plan& plan) noexcept
{
    return is_reversed
        ? highway_block_aligner::try_packed_global_gap<true>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, plan)
        : highway_block_aligner::try_packed_global_gap<false>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, plan);
}

/** Align one sequence region with the active fixed-target block kernel. */
template<sequence_alphabet p_alphabet>
[[nodiscard]] ksw_alignment_result align_block(
    bool is_reversed,
    bool is_diagonal_certificate_enabled,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_sequence,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const scoring_matrix<p_alphabet>& scoring_matrix,
    const block_aligner_parameters& parameters,
    block_aligner_execution_stats* execution_stats,
    block_aligner_construction_stats* construction_stats)
{
    if (is_reversed) {
        return is_diagonal_certificate_enabled
            ? highway_block_aligner::align<true, true>(
                query_sequence, target_sequence, query_start, query_end,
                target_start, target_end, scoring_matrix, parameters,
                execution_stats, construction_stats)
            : highway_block_aligner::align<true, false>(
                query_sequence, target_sequence, query_start, query_end,
                target_start, target_end, scoring_matrix, parameters,
                execution_stats, construction_stats);
    }
    return is_diagonal_certificate_enabled
        ? highway_block_aligner::align<false, true>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters,
            execution_stats, construction_stats)
        : highway_block_aligner::align<false, false>(
            query_sequence, target_sequence, query_start, query_end,
            target_start, target_end, scoring_matrix, parameters,
            execution_stats, construction_stats);
}

}  // namespace target
}  // namespace bioseq::simd
