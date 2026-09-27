/**
 * @file block_aligner_dp.cppm
 * @brief Compute alignment scores in moving SIMD blocks and record paths for traceback.
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

// Constants

/// Distance a block moves right or down, in DP cells.
inline constexpr size_t k_block_shift_step = 8;

/// Consecutive steps below the X-drop threshold required before stopping.
inline constexpr size_t k_x_drop_confirmation_count = 2;

/// Whether block shrinking is enabled.
inline constexpr bool k_block_shrink_enabled = true;

/// Border elements checked when deciding whether to shrink the block.
inline constexpr size_t k_block_shrink_suffix_length = k_block_shift_step / 4;

/// Bias keeping relative DP scores within int16 range. Subtract it when restoring full scores.
inline constexpr int16_t k_block_score_bias = static_cast<int16_t>(1 << 14);

/// Lowest stored DP score, used for unreachable cells.
inline constexpr int16_t k_block_score_minimum = 0;

/// Use at most sixteen int16 lanes. Supported targets have eight or sixteen.
using block_aligner_simd_tag = hn::CappedTag<block_aligner_score_type, 16>;
static_assert(std::same_as<hn::TFromD<block_aligner_simd_tag>, block_aligner_score_type>);

/** Work done by one Block Aligner call, including a successful diagonal shortcut. */
struct block_core_stats {
    std::uint64_t vector_batches = 0;  ///< DP vector batches evaluated.
    std::uint64_t cells = 0;  ///< DP cells evaluated.
    std::uint64_t diagonal_certificate_vector_batches = 0;  ///< SIMD batches used to check a certified diagonal.
    std::uint64_t diagonal_certificate_symbols = 0;  ///< Symbols compared when checking a certified diagonal.
    std::uint64_t grow_events = 0;  ///< Adaptive growth events.
    std::uint64_t shrink_events = 0;  ///< Adaptive shrink events.
    bool is_saturated = false;  ///< Whether a work counter saturated.
    bool is_x_drop_terminated = false;  ///< Whether X-drop terminated the run.
    bool is_diagonal_certified = false;  ///< Whether a diagonal check supplied the alignment result.

    /** Add to a counter and record overflow in is_saturated. */
    void add(std::uint64_t& destination, std::uint64_t increment = 1) noexcept {
        block_aligner_execution_stats::add(destination, increment, is_saturated);
    }

    /** Add a product to a counter, saturating if multiplication or addition overflows. */
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

/** Log an internal Block Aligner error and abort. */
[[noreturn]] inline void abort_block_aligner_invariant(
        std::string_view detail) {
    bioseq_log<logging_level::error>(
        "Block Aligner invariant failure: {}", detail);
    std::abort();
}

// Scalar helpers

/** Clamp a score to the range representable by int16_t. */
constexpr int16_t clamp_block_score_to_int16(int32_t score) {
    return static_cast<int16_t>(
        std::min<int32_t>(
            std::max<int32_t>(
                score, (std::numeric_limits<int16_t>::min)()),
            (std::numeric_limits<int16_t>::max)()));
}

/** Round integer division up. The denominator must be positive and the addition must fit. */
constexpr size_t divide_block_extent_round_up(
        size_t numerator,
        size_t denominator) {
    return (numerator + denominator - 1) / denominator;
}

// SIMD helpers

/// Join adjacent vector halves to shift scores by half a vector.
template<class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> shift_block_vector_half(
        p_simd_tag simd_tag,
        hn::Vec<p_simd_tag> lower_source,
        hn::Vec<p_simd_tag> upper_source) {
    return hn::ConcatLowerUpper(simd_tag, lower_source, upper_source);
}

/** Calculate buffer sizes for this CPU target, aborting if they cannot be represented. */
[[nodiscard]] inline block_aligner_storage_plan require_block_storage_plan(
    size_t query_length, size_t reference_length,
    size_t maximum_block_size, bool is_trace_enabled) {
    const auto plan = derive_block_aligner_storage_plan(
        query_length, reference_length, maximum_block_size,
        hn::Lanes(block_aligner_simd_tag{}), is_trace_enabled);
    if (!plan) {
        abort_block_aligner_invariant("Block storage geometry overflow");
    }
    return *plan;
}

// Padded sequence storage

/// Sequence ranks with a sentinel before the first symbol and padding after
/// the last. This lets SIMD loads and predecessor shifts reach across the
/// sequence edges safely. The first symbol is at index one.
///
/// The alignment arena owns the storage. This view must stay within its lifetime.
class block_padded_bytes {
public:
    /** Construct an empty padded input view. */
    block_padded_bytes() = default;

    /// Copy a sequence interval in the requested orientation and add sentinel padding.
    template<bool p_is_reversed, bool p_should_write_reversed, int p_alphabet_dimension,
             sequence_alphabet p_alphabet>
    void encode_region(
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            size_t region_begin,
            size_t region_end,
            size_t block_size) {
        if (region_end < region_begin) {
            abort_block_aligner_invariant("padded region endpoints are unordered");
        }
        length_ = region_end - region_begin;
        const auto storage_elements = block_padded_storage_elements(length_, block_size);
        if (!storage_elements) {
            abort_block_aligner_invariant("padded storage extent overflow");
        }
        data_ = context::current().alloc_align_array<uint8_t, 64>(*storage_elements);
        const uint8_t null_byte = static_cast<uint8_t>(p_alphabet_dimension);
        data_[0] = null_byte;
        sequence.template copy_ranks<
            p_is_reversed, p_should_write_reversed>(
                data_.subspan(1, length_), region_begin, region_end);
        std::memset(data_.data() + 1 + length_, null_byte, block_size);
    }

    /// Return the byte at a one-based padded index. Index zero is the sentinel.
    [[nodiscard]] uint8_t byte_at(size_t index) const { return data_[index]; }

    /// Pointer for SIMD loads at the requested padded-buffer index.
    [[nodiscard]] const uint8_t* data_at(size_t index) const {
        return data_.data() + index;
    }

    /// Number of bytes allocated for the padded sequence.
    [[nodiscard]] size_t storage_elements() const noexcept { return data_.size(); }

    /// Original (unpadded) sequence length.
    [[nodiscard]] size_t length() const { return length_; }

private:
    std::span<uint8_t> data_;
    size_t length_ = 0;
};

// Aligned DP score buffers

/// SIMD-aligned i16 scratch buffer owned by the current alignment arena.
class block_aligned_buffer {
public:
    /** Construct an empty aligned buffer. */
    block_aligned_buffer() = default;

    /** Allocate space for this many int16 scores in the alignment arena. */
    explicit block_aligned_buffer(size_t capacity)
        : storage_(context::current().alloc_align_array<block_aligner_score_type, 64>(capacity))
        , capacity_(capacity) {}

    /// Clear the requested prefix to the DP minimum. count must be a whole number of vectors.
    template<class p_simd_tag>
    void clear(p_simd_tag simd_tag, size_t count) {
        const auto minimum_scores = hn::Set(simd_tag, k_block_score_minimum);
        const size_t lane_count = hn::Lanes(simd_tag);
        for (size_t buffer_index = 0; buffer_index < count; buffer_index += lane_count) {
            hn::Store(minimum_scores, simd_tag, storage_.data() + buffer_index);
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

    /// Return a read-only pointer to the buffer.
    [[nodiscard]] const int16_t* data() const { return storage_.data(); }

private:
    std::span<block_aligner_score_type> storage_;
    size_t capacity_ = 0;
};

// Trace storage for CIGAR generation

/// Store DP decisions and block positions for CIGAR traceback.
/// The two arrays of packed decisions can grow to several MiB on long gaps.
///
/// All five arrays belong to the alignment arena. Clear the packed direction
/// bits before use. DP writes each other entry before traceback reads it.
class block_trace {
public:
    /// Packed trace word holding two decision bits per SIMD lane.
    using trace_type = uint32_t;

    /** Construct an empty trace buffer for score-only alignment. */
    block_trace() = default;

    /** Check the required sizes and allocate traceback storage. */
    block_trace(size_t query_length, size_t reference_length, size_t maximum_block_size)
        : block_trace(query_length, reference_length,
            require_block_storage_plan(query_length, reference_length, maximum_block_size, true)) {}

    /** Allocate traceback arrays using the sizes in the caller's storage plan. */
    block_trace(size_t query_length, size_t reference_length,
                const block_aligner_storage_plan& plan)
        : query_length_(query_length), reference_length_(reference_length) {
        if (!plan.is_trace_enabled) {
            abort_block_aligner_invariant("trace constructor requires traced storage geometry");
        }
        trace_capacity_ = plan.trace_capacity;
        auto& alignment_context = context::current();
        primary_trace_words_ = alignment_context.alloc_align_array<trace_type>(
            plan.trace_word_elements);
        secondary_trace_words_ = alignment_context.alloc_align_array<trace_type>(
            plan.trace_word_elements);
        block_directions_ = alignment_context.alloc_align_array<uint64_t>(
            plan.direction_word_elements);
        block_origins_ = alignment_context.alloc_align_array<uint32_t>(
            plan.coordinate_elements);
        block_extents_ = alignment_context.alloc_align_array<uint16_t>(
            plan.coordinate_elements);
        std::memset(block_directions_.data(), 0,
            plan.direction_word_elements * sizeof(uint64_t));
    }

    /** Reset counters and dimensions while keeping allocated storage. */
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
        // The constructor has checked capacity for the whole alignment.
        // Keep this per-vector check as a debug assertion. Allocation and
        // rectangle-level skipping still check their bounds at runtime.
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
        const size_t direction_word_index = block_count_ / 64;
        if (!is_block_slot_valid || direction_word_index >= block_directions_.size()
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
        const size_t direction_bit_index  = block_count_ % 64;
        block_directions_[direction_word_index] = (block_directions_[direction_word_index] & ~(1ULL << direction_bit_index))
                     | (static_cast<uint64_t>(is_right_block) << direction_bit_index);
        ++block_count_;
    }

    /** Leave space for an uncomputed part of a traced rectangle after early stopping. */
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
    /// Return the trace-word position, including space skipped within rectangles.
    [[nodiscard]] size_t trace_word_count() const { return trace_word_count_; }
    /// Return the number of written block records.
    [[nodiscard]] size_t block_count() const { return block_count_; }
    /// Return the query length used for this trace.
    [[nodiscard]] size_t query_length() const { return query_length_; }
    /// Return the reference length used for this trace.
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

// Traceback lookup table

/** DP state to follow while tracing the alignment backwards. */
enum class block_trace_table : uint8_t { diagonal = 0, column = 1, row = 2 };

/** One traceback step, including its CIGAR operation and next DP state. */
struct block_trace_lookup_entry {
    block_cigar_operation operation;  ///< Operation emitted by the transition.
    size_t query_step;  ///< Query symbols consumed.
    size_t reference_step;  ///< Reference symbols consumed.
    block_trace_table next_table;  ///< DP state to follow for the next step backwards.
};

/** Precompute how packed decisions map to CIGAR steps for rightward and downward blocks. */
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
                        // Downward blocks swap query and reference. Swap the gap directions
                        // and insertion/deletion operations when decoding their trace.
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
                        case 1: // Column gap after swapping query and reference.
                            entry = (secondary_trace_code & 0b10)
                                ? block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::diagonal}
                                : block_trace_lookup_entry{block_cigar_operation::deletion, 0, 1, block_trace_table::column};
                            break;
                        case 2: // Row gap after swapping query and reference.
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

/** Decode packed DP decisions into traceback steps. */
inline constexpr auto k_block_trace_lookup = build_block_trace_lookup();

// Adaptive-block working memory

/** Border scores, saved checkpoints and traceback storage owned by the alignment arena. */
struct block_aligner_scratch {
    block_trace trace;  ///< Optional packed traceback storage.
    block_aligned_buffer diagonal_column, gap_column, diagonal_row, gap_row;  ///< Scores along the current block's row and column borders.
    block_aligned_buffer diagonal_column_checkpoint, gap_column_checkpoint, diagonal_row_checkpoint, gap_row_checkpoint;  ///< Saved borders used when retrying with a larger block.
    block_aligned_buffer diagonal_shift_buffer, gap_shift_buffer;  ///< Newly computed border scores used when shifting the block.

    /** Calculate the buffer sizes once, then allocate the alignment scratch space. */
    block_aligner_scratch(size_t query_length, size_t reference_length,
                          size_t maximum_block_size, bool is_trace_enabled)
        : block_aligner_scratch(query_length, reference_length,
            require_block_storage_plan(
                query_length, reference_length, maximum_block_size, is_trace_enabled)) {}

    /** Allocate the buffers described by the storage plan. */
    block_aligner_scratch(size_t query_length, size_t reference_length,
                          const block_aligner_storage_plan& plan)
        : trace(plan.is_trace_enabled
            ? block_trace(query_length, reference_length, plan)
            : block_trace())
        , diagonal_column(plan.border_elements)
        , gap_column(plan.border_elements)
        , diagonal_row(plan.border_elements)
        , gap_row(plan.border_elements)
        , diagonal_column_checkpoint(plan.border_elements)
        , gap_column_checkpoint(plan.border_elements)
        , diagonal_row_checkpoint(plan.border_elements)
        , gap_row_checkpoint(plan.border_elements)
        , diagonal_shift_buffer(plan.shift_elements), gap_shift_buffer(plan.shift_elements) {}

    /** Clear current and saved borders, shift buffers and any traceback positions. */
    template<class p_simd_tag>
    void clear(
            p_simd_tag simd_tag,
            size_t query_length,
            size_t reference_length,
            size_t maximum_block_size,
            bool is_trace_enabled) {
        // Clear the full border capacity so a later checkpoint restore
        // cannot bring uninitialized scores into a larger block.
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
        diagonal_shift_buffer.clear(simd_tag, lane_count);
        gap_shift_buffer.clear(simd_tag, lane_count);
    }
};

// Parallel prefix maximum with accumulated gap cost

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
        for (size_t lane_index = 0; lane_index < lane_count / 2; ++lane_index) {
            corrected_costs[lane_index] = lane_values[lane_index];
        }
        for (size_t lane_index = lane_count / 2; lane_index < lane_count; ++lane_index) {
            corrected_costs[lane_index] = clamp_block_score_to_int16(
                static_cast<int32_t>(half_last) + static_cast<int32_t>(lane_values[lane_index]));
        }
        return {hn::Load(simd_tag, corrected_costs), four_lane_costs};
    }
    return {four_lane_costs, four_lane_costs};
}

/// Find the best row-gap score reaching each lane:
/// result[i] = max_{k=0..i} (input[k] + gap * (i-k)).
/// The shifts work within 128-bit groups. With sixteen lanes, carry the best
/// lower-half score into the upper half after scanning both groups.
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
        for (size_t lane_index = 0; lane_index < lane_count / 2; ++lane_index) corrected_values[lane_index] = k_block_score_minimum;
        for (size_t lane_index = lane_count / 2; lane_index < lane_count; ++lane_index) {
            corrected_values[lane_index] = clamp_block_score_to_int16(
                static_cast<int32_t>(lower_last)
                + static_cast<int32_t>(lane_offset_values[lane_index]));
        }
        return hn::Max(
            four_lane_scores,
            hn::Load(simd_tag, corrected_values));
    }
    return four_lane_scores;
}

// Pack DP decisions for traceback

/// Interleave two sets of sixteen decision bits into one trace word.
/// low_bits supplies the even bit positions and high_bits the odd positions,
/// giving each SIMD lane two adjacent bits.
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

/** Record the best predecessor and gap-opening decisions for each SIMD lane. */
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
    // Two bits per lane fit up to sixteen lanes in a uint32_t trace word.
    // A wider vector would need a wider trace word.
    HWY_DASSERT_M(lane_count <= 16, "Trace bitmask overflow: lane_count > 16 not supported");

    // Turn each lane's predecessor decision into a bit for the packed trace.
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

// Rectangular DP update

/// Compute a rectangle and save its border scores for the next block move.
/// Query positions fill SIMD lanes, while reference columns advance one at a
/// time. Downward moves reuse this calculation by swapping the sequences and
/// their border buffers.
///
/// Return each lane's maximum score. X-drop also records the row and column
/// offsets where that maximum was found.
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

    // Account for the first extension step when opening a row gap.
    const auto gap_open_adjustment = hn::SaturatedSub(gap_open_scores, gap_extension_scores);
    const auto minimum_scores = hn::Set(simd_tag, k_block_score_minimum);

    hn::Vec<p_simd_tag> diagonal_scores{}, row_gap_scores{};

    // Only the first block can contain the DP origin.
    const bool can_have_origin = (row_start == 0 && column_start == 0);

    size_t processed_columns = 0;
    for (size_t column_offset = 0; column_offset < width; ++column_offset) {
        auto previous_row_gap_scores = minimum_scores;
        diagonal_scores = minimum_scores;
        row_gap_scores = minimum_scores;
        [[maybe_unused]] auto previous_row_trace = minimum_scores;

        const uint8_t reference_rank = reference.byte_at(column_start + column_offset);
        const auto score_lookup = load_score_lookup_tables<p_alphabet>(
            score_matrix, simd_tag, reference_rank);

        // The origin can only land in the first processed column.
        const bool column_contains_origin = can_have_origin && (column_offset == 0);

        for (size_t row_offset = 0; row_offset < height; row_offset += lane_count) {
            auto previous_diagonal_scores = hn::Load(simd_tag, diagonal_column + row_offset);
            auto previous_column_gap_scores = hn::Load(simd_tag, gap_column + row_offset);

            // Bring each diagonal predecessor into the lane for its successor.
            auto diagonal_predecessor_scores = shift_lanes_left_with_predecessor(simd_tag, previous_diagonal_scores, diagonal_corner);
            diagonal_corner = previous_diagonal_scores;

            // Reuse this reference column's scoring lookup across the query lanes.
            auto scores = get_scores_with_lookup_tables<p_alphabet>(
                score_matrix,
                simd_tag,
                score_lookup,
                query.data_at(row_start + row_offset));

            // Score the diagonal match or mismatch.
            diagonal_scores = hn::SaturatedAdd(diagonal_predecessor_scores, scores);

            // Start the alignment at score zero in the first cell, expressed
            // using the current score bias.
            if (column_contains_origin && row_offset == 0) {
                diagonal_scores = replace_first_lanes(simd_tag, diagonal_scores, relative_zero);
            }

            // Open or extend a gap from the previous column.
            auto opened_column_gap_scores = hn::SaturatedAdd(previous_diagonal_scores, gap_open_scores);
            auto column_gap_scores = hn::Max(hn::SaturatedAdd(previous_column_gap_scores, gap_extension_scores), opened_column_gap_scores);
            diagonal_scores = hn::Max(diagonal_scores, column_gap_scores);

            // Propagate row-gap scores across the SIMD lanes.
            auto opened_row_gap_scores = hn::SaturatedAdd(
                diagonal_scores, gap_open_adjustment);
            row_gap_scores = scan_block_prefix(
                simd_tag,
                opened_row_gap_scores,
                gap_extension_scores,
                prefix_scan_offsets);

            // Also allow a row gap to continue from the preceding vector.
            row_gap_scores = hn::Max(row_gap_scores, hn::SaturatedAdd(
                broadcast_last_lane(simd_tag, previous_row_gap_scores), accumulated_gap_extension));
            diagonal_scores = hn::Max(diagonal_scores, row_gap_scores);
            previous_row_gap_scores = row_gap_scores;

            // Save predecessor choices for traceback.
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

            // Keep each lane's best score and, for X-drop, its position.
            maximum_scores = hn::Max(maximum_scores, diagonal_scores);
            if constexpr (p_is_x_drop_enabled) {
                auto update_mask = hn::MaskFromVec(
                    hn::VecFromMask(simd_tag, hn::Eq(maximum_scores, diagonal_scores)));
                maximum_row_offsets = hn::IfThenElse(update_mask,
                    hn::Set(simd_tag, static_cast<int16_t>(row_offset)), maximum_row_offsets);
                maximum_column_offsets = hn::IfThenElse(update_mask,
                    hn::Set(simd_tag, static_cast<int16_t>(column_offset)), maximum_column_offsets);
            }

            // Store updated column borders
            hn::Store(diagonal_scores, simd_tag, diagonal_column + row_offset);
            hn::Store(column_gap_scores, simd_tag, gap_column + row_offset);
        }

        // Save the bottom row for the next block move.
        diagonal_corner = hn::Set(simd_tag, k_block_score_minimum);
        diagonal_row[column_offset] = extract_last_lane(simd_tag, diagonal_scores);
        gap_row[column_offset] = extract_last_lane(simd_tag, row_gap_scores);
        processed_columns = column_offset + 1;

        // Global alignment can stop this rectangle once it passes both sequence ends.
        if constexpr (!p_is_x_drop_enabled) {
            if (row_start + height > query.length() && column_start + column_offset >= reference.length()) {
                if constexpr (p_is_trace_enabled) {
                    trace.skip_trace_words((width - 1 - column_offset) * (height / lane_count));
                }
                break;
            }
        }
    }

    if constexpr (p_should_collect_stats) {
        // Count only completed columns, including an early stop. Updating
        // once per rectangle keeps counter work out of the per-vector loop.
        core_stats.add_product(
            core_stats.vector_batches,
            processed_columns,
            height / lane_count);
        core_stats.add_product(core_stats.cells, processed_columns, height);
    }

    return {maximum_scores, maximum_row_offsets, maximum_column_offsets};
}

// Helpers for moving and resizing blocks

/// Move the borders by one block step and adjust their score bias.
/// Append the newly computed edge and return the corner needed after a turn.
template<class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> shift_block_scores_and_offset(
        p_simd_tag simd_tag,
        size_t block_size,
        block_aligned_buffer& diagonal_buffer,
        block_aligned_buffer& gap_buffer,
        block_aligned_buffer& diagonal_shift_buffer,
        block_aligned_buffer& gap_shift_buffer,
        hn::Vec<p_simd_tag> score_offset_adjustment)
{
    const size_t lane_count = hn::Lanes(simd_tag);
    auto current_diagonal_scores = hn::SaturatedAdd(hn::Load(simd_tag, diagonal_buffer.data()), score_offset_adjustment);
    auto current_gap_scores = hn::SaturatedAdd(hn::Load(simd_tag, gap_buffer.data()), score_offset_adjustment);

    HWY_ALIGN int16_t lane_values[hn::MaxLanes(simd_tag)];
    hn::Store(current_diagonal_scores, simd_tag, lane_values);
    const auto diagonal_corner = hn::Set(simd_tag, lane_values[k_block_shift_step - 1]);

    if (lane_count == k_block_shift_step) {
        // Eight lanes: move one whole vector.
        for (size_t buffer_index = 0; buffer_index + lane_count < block_size; buffer_index += lane_count) {
            auto next_diagonal_scores = hn::SaturatedAdd(hn::Load(simd_tag, diagonal_buffer.data() + buffer_index + lane_count), score_offset_adjustment);
            auto next_gap_scores = hn::SaturatedAdd(hn::Load(simd_tag, gap_buffer.data() + buffer_index + lane_count), score_offset_adjustment);
            hn::Store(next_diagonal_scores, simd_tag, diagonal_buffer.data() + buffer_index);
            hn::Store(next_gap_scores, simd_tag, gap_buffer.data() + buffer_index);
            current_diagonal_scores = next_diagonal_scores; current_gap_scores = next_gap_scores;
        }
        hn::Store(hn::Load(simd_tag, diagonal_shift_buffer.data()), simd_tag, diagonal_buffer.data() + block_size - lane_count);
        hn::Store(hn::Load(simd_tag, gap_shift_buffer.data()), simd_tag, gap_buffer.data() + block_size - lane_count);
    } else {
        // Sixteen lanes: join vector halves to move eight scores.
        for (size_t buffer_index = 0; buffer_index + lane_count < block_size; buffer_index += lane_count) {
            auto next_diagonal_scores = hn::SaturatedAdd(hn::Load(simd_tag, diagonal_buffer.data() + buffer_index + lane_count), score_offset_adjustment);
            auto next_gap_scores = hn::SaturatedAdd(hn::Load(simd_tag, gap_buffer.data() + buffer_index + lane_count), score_offset_adjustment);
            hn::Store(shift_block_vector_half(simd_tag, next_diagonal_scores, current_diagonal_scores), simd_tag, diagonal_buffer.data() + buffer_index);
            hn::Store(shift_block_vector_half(simd_tag, next_gap_scores, current_gap_scores), simd_tag, gap_buffer.data() + buffer_index);
            current_diagonal_scores = next_diagonal_scores; current_gap_scores = next_gap_scores;
        }
        hn::Store(shift_block_vector_half(simd_tag, hn::Load(simd_tag, diagonal_shift_buffer.data()), current_diagonal_scores),
                  simd_tag, diagonal_buffer.data() + block_size - lane_count);
        hn::Store(shift_block_vector_half(simd_tag, hn::Load(simd_tag, gap_shift_buffer.data()), current_gap_scores),
                  simd_tag, gap_buffer.data() + block_size - lane_count);
    }
    return diagonal_corner;
}

/// Apply a score-bias adjustment to two complete border buffers.
template<class p_simd_tag>
HWY_INLINE void offset_block_scores(
        p_simd_tag simd_tag,
        size_t block_size,
        block_aligned_buffer& diagonal_buffer,
        block_aligned_buffer& gap_buffer,
        hn::Vec<p_simd_tag> score_offset_adjustment)
{
    const size_t lane_count = hn::Lanes(simd_tag);
    for (size_t buffer_index = 0; buffer_index < block_size; buffer_index += lane_count) {
        hn::Store(hn::SaturatedAdd(hn::Load(simd_tag, diagonal_buffer.data() + buffer_index), score_offset_adjustment),
                  simd_tag, diagonal_buffer.data() + buffer_index);
        hn::Store(hn::SaturatedAdd(hn::Load(simd_tag, gap_buffer.data() + buffer_index), score_offset_adjustment),
                  simd_tag, gap_buffer.data() + buffer_index);
    }
}

/// Find the best score near the start of a border to choose the next direction.
template<class p_simd_tag>
HWY_INLINE int16_t block_prefix_maximum(
        p_simd_tag simd_tag,
        const int16_t* buffer) {
    HWY_ALIGN int16_t lane_values[hn::MaxLanes(simd_tag)];
    hn::Store(hn::Load(simd_tag, buffer), simd_tag, lane_values);
    int16_t result = k_block_score_minimum;
    for (size_t lane_index = 0; lane_index < k_block_shift_step; ++lane_index) result = std::max(result, lane_values[lane_index]);
    return result;
}

/// Find the best score near the end of a border to decide whether shrinking is safe.
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
    for (size_t lane_index = lane_count - k_block_shrink_suffix_length; lane_index < lane_count; ++lane_index)
        result = std::max(result, lane_values[lane_index]);
    return result;
}

// Main adaptive-block alignment loop

/** Move right, move down or expand the block at a saved position. */
enum class block_direction : uint8_t { right, down, grow };

/**
 * Align a region with a moving DP block.
 *
 * Follow the stronger border. Save improving positions so a stalled search
 * can retry with a larger block. Shrink when trailing edges contain the best score.
 * Reuse border scores with an offset for int16 SIMD arithmetic.
 *
 * Global mode reaches both ends. X-drop returns the best extension after two
 * consecutive steps below its threshold. Output positions count consumed symbols.
 */
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
                    scratch.diagonal_shift_buffer.data(), scratch.gap_shift_buffer.data(),
                    diagonal_corner_input, clamp_block_score_to_int16(-score_offset + k_block_score_bias), true,
                    core_stats);

            right_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_column.data());
            diagonal_corner = shift_block_scores_and_offset(simd_tag, block_size,
                scratch.diagonal_row, scratch.gap_row,
                scratch.diagonal_shift_buffer, scratch.gap_shift_buffer, score_offset_adjustment);
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

            // Swap sequences and borders so the same rectangle update can move downward.
            std::tie(maximum_scores, maximum_row_offsets, maximum_column_offsets) =
                place_alignment_block<p_is_trace_enabled, p_is_x_drop_enabled, p_should_collect_stats, p_alphabet>(
                    simd_tag, score_matrix, negative_gap_open, negative_gap_extension,
                    reference, query, scratch.trace,
                    block_column_start, block_row_start + block_size - k_block_shift_step,
                    k_block_shift_step, block_size,
                    scratch.diagonal_row.data(), scratch.gap_row.data(),
                    scratch.diagonal_shift_buffer.data(), scratch.gap_shift_buffer.data(),
                    diagonal_corner_input, clamp_block_score_to_int16(-score_offset + k_block_score_bias), false,
                    core_stats);

            down_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_row.data());
            diagonal_corner = shift_block_scores_and_offset(simd_tag, block_size,
                scratch.diagonal_column, scratch.gap_column,
                scratch.diagonal_shift_buffer, scratch.gap_shift_buffer, score_offset_adjustment);
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

            // Add the rows below the previous block.
            hn::Vec<p_simd_tag> down_growth_maximum_scores, down_growth_row_offsets, down_growth_column_offsets;
            std::tie(down_growth_maximum_scores, down_growth_row_offsets, down_growth_column_offsets) =
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
            growth_maximum_row_offsets = down_growth_row_offsets;
            growth_maximum_column_offsets = down_growth_column_offsets;

            if constexpr (p_is_trace_enabled) {
                scratch.trace.append_block(block_row_start, block_column_start + previous_block_size,
                    grow_step, block_size, true);
            }

            // Add the columns on the right, including the new lower corner.
            hn::Vec<p_simd_tag> right_growth_maximum_scores, right_growth_row_offsets, right_growth_column_offsets;
            std::tie(right_growth_maximum_scores, right_growth_row_offsets, right_growth_column_offsets) =
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
            growth_maximum_scores = down_growth_maximum_scores;

            // Save the expanded borders and trace position for another growth attempt.
            for (size_t buffer_index = 0; buffer_index < block_size; buffer_index += lane_count) {
                scratch.diagonal_column_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_column, buffer_index);
                scratch.gap_column_checkpoint.copy_vector_from(simd_tag, scratch.gap_column, buffer_index);
                scratch.diagonal_row_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_row, buffer_index);
                scratch.gap_row_checkpoint.copy_vector_from(simd_tag, scratch.gap_row, buffer_index);
            }
            if constexpr (p_is_trace_enabled) { scratch.trace.save_checkpoint(); }

            maximum_scores = right_growth_maximum_scores; maximum_row_offsets = right_growth_row_offsets; maximum_column_offsets = right_growth_column_offsets;
            break;
        }
        } // switch

        // Convert the block maximum back to the full score.
        previous_direction = direction;
        int16_t maximum_score = hn::ReduceMax(simd_tag, maximum_scores);
        int16_t growth_maximum_score = hn::ReduceMax(simd_tag, growth_maximum_scores);
        int16_t current_maximum_score = std::max(maximum_score, growth_maximum_score);
        maximum_score_offset = score_offset + static_cast<int32_t>(current_maximum_score) - static_cast<int32_t>(k_block_score_bias);

        growth_delay_iterations += 1;
        bool should_grow_without_maximum = (direction == block_direction::grow);

        if (maximum_score_offset > best_score) {
            if constexpr (p_is_x_drop_enabled) {
                // Remember where the new best extension score was found.
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
                for (size_t buffer_index = 0; buffer_index < block_size; buffer_index += lane_count) {
                    scratch.diagonal_column_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_column, buffer_index);
                    scratch.gap_column_checkpoint.copy_vector_from(simd_tag, scratch.gap_column, buffer_index);
                    scratch.diagonal_row_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_row, buffer_index);
                    scratch.gap_row_checkpoint.copy_vector_from(simd_tag, scratch.gap_row, buffer_index);
                }
                if constexpr (p_is_trace_enabled) { scratch.trace.save_checkpoint(); }
                should_grow_without_maximum = false;
            }
            best_score = maximum_score_offset;
            growth_delay_iterations = 0;
        }

        // Stop after two consecutive steps beyond the X-drop threshold.
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

        // Stop once the block covers both sequence ends.
        if (block_row_start + block_size > query.length()
            && block_column_start + block_size > reference.length()) {
            break;
        }

        // If one sequence has ended, keep advancing along the other.
        if (block_column_start + block_size > reference.length()) {
            block_row_start += k_block_shift_step; direction = block_direction::down; continue;
        }
        if (block_row_start + block_size > query.length()) {
            block_column_start += k_block_shift_step; direction = block_direction::right; continue;
        }

        // Retry from the checkpoint with a larger block when the score stops improving.
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
                for (size_t buffer_index = 0; buffer_index < previous_block_size; buffer_index += lane_count) {
                    scratch.diagonal_column.copy_vector_from(simd_tag, scratch.diagonal_column_checkpoint, buffer_index);
                    scratch.gap_column.copy_vector_from(simd_tag, scratch.gap_column_checkpoint, buffer_index);
                    scratch.diagonal_row.copy_vector_from(simd_tag, scratch.diagonal_row_checkpoint, buffer_index);
                    scratch.gap_row.copy_vector_from(simd_tag, scratch.gap_row_checkpoint, buffer_index);
                }
                if constexpr (p_is_trace_enabled) { scratch.trace.restore_checkpoint(); }
                growth_delay_iterations = 0;
                continue;
            }
        }

        // Shrink when the trailing edges still contain the current best score.
        if constexpr (k_block_shrink_enabled) {
            if (block_size > minimum_block_size && growth_delay_iterations == 0) {
                int16_t shrink_border_maximum = std::max(
                    block_suffix_maximum(simd_tag, scratch.diagonal_row.data(), block_size),
                    block_suffix_maximum(simd_tag, scratch.diagonal_column.data(), block_size));
                if (shrink_border_maximum >= current_maximum_score) {
                    previous_direction = block_direction::grow; // The old corner is outside the smaller block.
                    block_size /= 2;
                    if constexpr (p_should_collect_stats) {
                        core_stats.add(core_stats.shrink_events);
                    }
                    for (size_t buffer_index = 0; buffer_index < block_size; buffer_index += lane_count) {
                        scratch.diagonal_column.copy_vector(simd_tag, buffer_index, buffer_index + block_size);
                        scratch.gap_column.copy_vector(simd_tag, buffer_index, buffer_index + block_size);
                        scratch.diagonal_row.copy_vector(simd_tag, buffer_index, buffer_index + block_size);
                        scratch.gap_row.copy_vector(simd_tag, buffer_index, buffer_index + block_size);
                    }
                    block_row_start += block_size; block_column_start += block_size;
                    checkpoint_row_start = block_row_start; checkpoint_column_start = block_column_start; checkpoint_score_offset = score_offset;
                    for (size_t buffer_index = 0; buffer_index < block_size; buffer_index += lane_count) {
                        scratch.diagonal_column_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_column, buffer_index);
                        scratch.gap_column_checkpoint.copy_vector_from(simd_tag, scratch.gap_column, buffer_index);
                        scratch.diagonal_row_checkpoint.copy_vector_from(simd_tag, scratch.diagonal_row, buffer_index);
                        scratch.gap_row_checkpoint.copy_vector_from(simd_tag, scratch.gap_row, buffer_index);
                    }
                    right_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_column.data());
                    down_border_maximum = block_prefix_maximum(simd_tag, scratch.diagonal_row.data());
                    if constexpr (p_is_trace_enabled) { scratch.trace.save_checkpoint(); }
                    growth_delay_iterations = 0;
                }
            }
        }

        // Move toward the better border. Choose right on a tie.
        if (down_border_maximum > right_border_maximum) {
            block_row_start += k_block_shift_step; direction = block_direction::down;
        } else {
            block_column_start += k_block_shift_step; direction = block_direction::right;
        }
    } // main loop

    // Return the best extension or the global alignment endpoint.
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
