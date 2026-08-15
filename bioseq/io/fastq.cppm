/**
 * @file fastq.cppm
 * @brief Per-record FASTQ FSM driver. kseqpp-port behaviour with
 *        Highway-backed `\n` scans hidden behind the
 *        `simd::scan_first_byte` non-inline wrapper.
 *
 * Header bytes flow directly into the batch's
 * shared `_header_storage`, sequence words into `_sequence_storage`,
 * quality bytes into `_quality_storage`. The emit-record callback
 * only records the per-record offset / padding tuple. Per-record
 * heap allocation count: 0.
 *
 * The state machine is the textbook FASTQ shape extended for the two
 * FASTQ edge cases:
 *
 *   1. **Multi-line bodies.** Both the sequence and the quality may
 *      span multiple `\n`-separated lines. The body loop re-enters
 *      the sequence-line state for each non-`+` line until it sees a `+`.
 *      The qual loop accumulates until
 *      `quality.size() == sequence_letters`.
 *
 *   2. **Byte-count terminator on quality.** Quality lines may legally
 *      start with `@`, `+`, or `>`. We MUST NOT use a sentinel-based
 *      terminator on qual; instead we count stripped-CR bytes.
 *
 * `strict_fastq` (default true) controls how the FSM reacts to a
 * missing or truncated quality section.
 *
 * Highway is included directly so the FSM can call
 * `bioseq::simd::active_target::encode_body_chunk*` without a
 * translation layer (mirrors `fasta.cppm`).
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

export module bioseq.io.fastq;

import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.simd;
import bioseq.io.fasta;  // reuse parse_outcome enum

export namespace bioseq::io {

/// FASTQ per-record FSM progress marker.
enum class fastq_parser_progress : std::uint8_t {
    at_header_sigil = 0,    ///< Expect '@'.
    in_header,              ///< Accumulating header bytes.
    at_body_line_start,     ///< Peeking the first byte of the next body line.
    in_sequence_line,       ///< Mid-sequence-line scan to next '\n'.
    at_separator_line,      ///< Just consumed '+', skipping comment to '\n'.
    in_quality_line,        ///< Mid-quality-line scan; accumulating bytes.
    finished_record,        ///< Ready for emit.
};

/// Per-record FASTQ parser state. Aligned to a cache line
/// so a future multi-parser pipeline can store one state per parser
/// thread without false-sharing the partial-encoding fields.
template <typename p_storage_unit,
          std::size_t p_bits_per_char,
          std::size_t p_letters_per_unit>
struct alignas(64) fastq_parser_state {
    fastq_parser_progress progress = fastq_parser_progress::at_header_sigil;
    std::size_t header_start_offset = 0;
    std::size_t quality_start_offset = 0;
    bioseq::simd::active_target::encode_state<
        p_storage_unit,
        p_bits_per_char,
        p_letters_per_unit> body{};
    /// Raw byte count of sequence lines after newline stripping. This
    /// is the value FASTQ qual must match — qual is one byte per
    /// position, regardless of whether the position is a canonical
    /// base or a skip-sentinel like `N`. Tracked independently of
    /// `body.total_letters` because the encoder counts only
    /// non-skip letters; dna5 inputs (with `N`) have
    /// `body.total_letters < sequence_byte_count`.
    std::size_t sequence_byte_count = 0;
    /// Snapshot of `sequence_byte_count` taken at the seq → '+'
    /// transition. Compared against qual_size to terminate qual
    /// accumulation at the right byte count.
    std::size_t sequence_letter_count = 0;
    /// Borrowed pointers to the batch's shared storage.
    std::vector<char>* header_storage = nullptr;
    std::vector<std::uint8_t>* quality_storage = nullptr;

    void reset_for_next_record() noexcept {
        progress = fastq_parser_progress::at_header_sigil;
        body.reset();
        sequence_byte_count = 0;
        sequence_letter_count = 0;
    }

    void attach_to_batch(std::vector<char>& header_buf,
                         std::vector<p_storage_unit>& seq_buf,
                         std::vector<std::uint8_t>& quality_buf) noexcept {
        header_storage = &header_buf;
        body.output_storage = &seq_buf;
        quality_storage = &quality_buf;
    }
};

/// Parse one FASTQ record from the slab. Multi-line bodies and
/// multi-line quality strings are both supported. Spec §10.
///
/// `emit_record` is invoked with
/// `(header_start, header_size, seq_start, seq_word_count, padding,
///   qual_start, qual_size)` — offsets into the batch's shared
/// storage plus the encoder's `(word_count, padding)` summary.
template <
    sequence_alphabet p_alphabet,
    typename p_storage_unit = std::uint64_t,
    typename p_emit_record,
    typename p_failure>
[[nodiscard]] record_parse_result parse_one_fastq_record(
    fastq_parser_state<p_storage_unit,
        compact_sequence_range<sequence_custom_alphabet<
            p_alphabet,
            p_storage_unit>>::traits::bits_per_char,
        compact_sequence_range<sequence_custom_alphabet<
            p_alphabet,
            p_storage_unit>>::traits::letters_per_unit>& state,
    const std::uint8_t* slab_data,
    std::size_t slab_size,
    bool is_eof,
    std::size_t cursor,
    std::size_t global_offset,
    bool strict_fastq,
    p_emit_record&& emit_record,
    p_failure&& failure_fn) {

    using sequence_traits_type =
        typename compact_sequence_range<
            sequence_custom_alphabet<p_alphabet, p_storage_unit>>::traits;
    constexpr std::size_t bits_per_char = sequence_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits_type::letters_per_unit;

    DEBUG_ASSERT(state.header_storage != nullptr,
                 "fastq_parser_state must be attached to a batch via attach_to_batch");
    DEBUG_ASSERT(state.body.output_storage != nullptr,
                 "fastq_parser_state must be attached to a batch via attach_to_batch");
    DEBUG_ASSERT(state.quality_storage != nullptr,
                 "fastq_parser_state must be attached to a batch via attach_to_batch");

    std::size_t parse_cursor = cursor;

    auto need_more = [&parse_cursor]() -> record_parse_result {
        return record_parse_result{parse_outcome::incomplete, parse_cursor};
    };

    // ----- Step 1: AT_HEADER_SIGIL — expect '@' -----
    if (state.progress == fastq_parser_progress::at_header_sigil) {
        if (parse_cursor >= slab_size) {
            return record_parse_result{
                is_eof ? parse_outcome::at_end_of_file : parse_outcome::incomplete, parse_cursor};
        }
        if (slab_data[parse_cursor] != '@') {
            emit_failure(failure_fn,
                         "expected '@' at file position {}, got byte 0x{:02x}",
                         global_offset + parse_cursor,
                         static_cast<unsigned>(slab_data[parse_cursor]));
            return record_parse_result{parse_outcome::abort, parse_cursor};
        }
        // Snapshot record-start offsets for emit_record.
        state.header_start_offset = state.header_storage->size();
        state.body.record_start_offset = state.body.output_storage->size();
        state.quality_start_offset = state.quality_storage->size();
        parse_cursor += 1;
        state.progress = fastq_parser_progress::in_header;
    }

    // ----- Step 2: IN_HEADER — scan to '\n' -----
    if (state.progress == fastq_parser_progress::in_header) {
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes) {
            state.header_storage->insert(
                state.header_storage->end(),
                reinterpret_cast<const char*>(slab_data + parse_cursor),
                reinterpret_cast<const char*>(slab_data + parse_cursor + remaining_bytes));
            parse_cursor = slab_size;
            return need_more();
        }
        std::size_t header_size = newline_offset;
        if (header_size > 0 &&
            slab_data[parse_cursor + header_size - 1] == '\r') {
            header_size -= 1;
        }
        state.header_storage->insert(
            state.header_storage->end(),
            reinterpret_cast<const char*>(slab_data + parse_cursor),
            reinterpret_cast<const char*>(slab_data + parse_cursor + header_size));
        parse_cursor += newline_offset + 1;
        state.progress = fastq_parser_progress::at_body_line_start;
    }

    // ----- Step 3: body lines — accumulate seq lines until '+' -----
    while (state.progress == fastq_parser_progress::at_body_line_start ||
           state.progress == fastq_parser_progress::in_sequence_line) {
        if (state.progress == fastq_parser_progress::at_body_line_start) {
            if (parse_cursor >= slab_size) return need_more();
            const std::uint8_t leading_byte = slab_data[parse_cursor];
            if (leading_byte == '+') {
                state.sequence_letter_count = state.sequence_byte_count;
                state.progress = fastq_parser_progress::at_separator_line;
                break;
            }
            if (leading_byte == '@' || leading_byte == '>') {
                state.sequence_letter_count = state.sequence_byte_count;
                if (strict_fastq) {
                    emit_failure(failure_fn,
                                 "FASTQ record without quality at file position {}",
                                 global_offset + parse_cursor);
                    return record_parse_result{parse_outcome::abort, parse_cursor};
                }
                state.progress = fastq_parser_progress::finished_record;
                break;
            }
            state.progress = fastq_parser_progress::in_sequence_line;
        }

        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes) {
            return need_more();
        }
        std::size_t line_size = newline_offset;
        if (line_size > 0 &&
            slab_data[parse_cursor + line_size - 1] == '\r') {
            line_size -= 1;
        }
        bioseq::simd::active_target::encode_body_chunk<
            p_alphabet,
            bioseq::simd::active_target::unknown_byte_policy::drop,
            p_storage_unit, bits_per_char, letters_per_unit>(
                state.body, slab_data + parse_cursor, line_size);
        state.sequence_byte_count += line_size;
        parse_cursor += newline_offset + 1;
        state.progress = fastq_parser_progress::at_body_line_start;
    }

    // ----- Step 4: AT_SEP_LINE — consume '+...\n' -----
    if (state.progress == fastq_parser_progress::at_separator_line) {
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes) return need_more();
        parse_cursor += newline_offset + 1;
        state.progress = fastq_parser_progress::in_quality_line;
    }

    // ----- Step 5: IN_QUAL_LINE — multi-line qual accumulation -----
    while (state.progress == fastq_parser_progress::in_quality_line) {
        if (parse_cursor >= slab_size) {
            if (is_eof) {
                const std::size_t qual_size = state.quality_storage->size()
                                            - state.quality_start_offset;
                if (qual_size < state.sequence_letter_count) {
                    emit_failure(failure_fn,
                                 "truncated FASTQ record (qual={}, seq={}) at file position {}",
                                 qual_size,
                                 state.sequence_letter_count,
                                 global_offset + parse_cursor);
                    return record_parse_result{parse_outcome::abort, parse_cursor};
                }
                state.progress = fastq_parser_progress::finished_record;
                break;
            }
            return need_more();
        }
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + parse_cursor, remaining_bytes);

        if (newline_offset == remaining_bytes) {
            if (is_eof) {
                std::size_t line_size = remaining_bytes;
                if (line_size > 0 &&
                    slab_data[parse_cursor + line_size - 1] == '\r') {
                    line_size -= 1;
                }
                state.quality_storage->insert(
                    state.quality_storage->end(),
                    slab_data + parse_cursor,
                    slab_data + parse_cursor + line_size);
                parse_cursor += remaining_bytes;
                const std::size_t qual_size = state.quality_storage->size()
                                            - state.quality_start_offset;
                if (qual_size < state.sequence_letter_count) {
                    emit_failure(failure_fn,
                                 "truncated FASTQ record (qual={}, seq={}) at file position {}",
                                 qual_size,
                                 state.sequence_letter_count,
                                 global_offset + parse_cursor);
                    return record_parse_result{parse_outcome::abort, parse_cursor};
                }
                state.progress = fastq_parser_progress::finished_record;
                break;
            }
            return need_more();
        }
        std::size_t line_size = newline_offset;
        if (line_size > 0 &&
            slab_data[parse_cursor + line_size - 1] == '\r') {
            line_size -= 1;
        }
        state.quality_storage->insert(
            state.quality_storage->end(),
            slab_data + parse_cursor,
            slab_data + parse_cursor + line_size);
        parse_cursor += newline_offset + 1;
        const std::size_t qual_size = state.quality_storage->size()
                                    - state.quality_start_offset;
        if (qual_size >= state.sequence_letter_count) {
            if (qual_size > state.sequence_letter_count) {
                emit_failure(failure_fn,
                             "FASTQ qual length mismatch (qual={}, seq={}) at file position {}",
                             qual_size,
                             state.sequence_letter_count,
                             global_offset + parse_cursor);
                return record_parse_result{parse_outcome::abort, parse_cursor};
            }
            state.progress = fastq_parser_progress::finished_record;
            break;
        }
    }

    // ----- Step 6: FINISHED_RECORD — finalise + emit -----
    if (state.progress == fastq_parser_progress::finished_record) {
        const std::size_t header_size =
            state.header_storage->size() - state.header_start_offset;
        const std::size_t seq_start = state.body.record_start_offset;
        const auto [seq_word_count, padding] =
            bioseq::simd::active_target::finalize_encode_state<p_storage_unit,
                                                  bits_per_char,
                                                  letters_per_unit>(state.body);
        const std::size_t qual_size = state.quality_storage->size()
                                    - state.quality_start_offset;
        emit_record(state.header_start_offset,
                    header_size,
                    seq_start,
                    seq_word_count,
                    padding,
                    state.quality_start_offset,
                    qual_size);
        state.reset_for_next_record();
        return record_parse_result{parse_outcome::complete, parse_cursor};
    }

    return need_more();
}

}  // namespace bioseq::io
