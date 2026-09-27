/**
 * @file block_aligner_traceback.cppm
 * @brief Reconstructs adaptive-block alignments from recorded SIMD trace data.
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

export module bioseq.highway.sources.block_aligner_traceback;

import std;
import bioseq.sequences.cigar;
import bioseq.alignment.contracts;
import bioseq.highway.sources.block_aligner_dp;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// Traceback — CIGAR generation from recorded trace data
// =============================================================================

/** Packed CIGAR and optional edit count produced by traceback. */
struct block_traceback_result {
    alignment_result_vector<std::uint32_t> cigar;  ///< Packed KSW2-compatible operations.
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
    const size_t cigar_step_bound = query_end_position + reference_end_position;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    cigar.set_logical_bounds(cigar_step_bound, cigar_step_bound);
#endif
    cigar.reserve(cigar_step_bound);

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

}  // namespace target
}  // namespace bioseq::simd
