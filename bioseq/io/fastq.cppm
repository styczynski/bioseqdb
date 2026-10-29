/**
 * @file fastq.cppm
 * @brief Incremental FASTQ record parser.
 *
 * Multi-line sequence fields and requested quality fields are appended directly
 * to batch-owned storage. Quality completion is determined by byte count, so
 * marker characters at the start of a quality line remain data.
 */
module;

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <string>
#include <utility>
#include <vector>

export module bioseq.io.fastq;

import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.simd;
import bioseq.io.fasta; // reuse parse_outcome enum

export namespace bioseq::io {

/// Position within a FASTQ record, kept across input refills.
enum class fastq_parser_progress : std::uint8_t {
    at_header_sigil = 0, ///< Expect '@'.
    in_header,           ///< Accumulating header bytes.
    at_body_line_start,  ///< Peeking the first byte of the next body line.
    in_sequence_line,    ///< Mid-sequence-line scan to next '\n'.
    at_separator_line,   ///< At the '+' separator, waiting for its line to end.
    in_quality_line,     ///< Mid-quality-line scan. Accumulating bytes.
    finished_record,     ///< Ready to emit the completed record.
};

/// State for one FASTQ tecord, aligned to keep concurrent parser states apart.
template <typename p_storage_unit, std::size_t p_bits_per_char, std::size_t p_letters_per_unit>
struct alignas(64) fastq_parser_state {
    fastq_parser_progress progress = fastq_parser_progress::at_header_sigil;
    std::size_t header_start_offset = 0;
    std::size_t quality_start_offset = 0;
    bioseq::simd::active_target::encode_state<p_storage_unit, p_bits_per_char, p_letters_per_unit>
        body{};
    /// Sequence bytes excluding line endings, used as the expected quality length.
    /// Strict encoding preserves one accepted symbol per input residue byte.
    std::size_t sequence_byte_count = 0;
    /// Expected quality length saved when sequence parsing ends.
    std::size_t sequence_letter_count = 0;
    /// Quality bytes excluding line endings, independent of retained storage.
    std::size_t quality_byte_count = 0;
    /// Retain batch quality bytes; sequence-only streams disable this before parsing.
    /// This policy survives record resets and storage reattachment.
    bool retain_quality = true;
    /// Borrowed pointers to the batch's shared storage.
    std::vector<char>* header_storage = nullptr;
    std::vector<std::uint8_t>* quality_storage = nullptr;

    /// Reset record-local progress while keeping attached batch storage.
    void reset_for_next_record() noexcept {
        progress = fastq_parser_progress::at_header_sigil;
        body.reset();
        sequence_byte_count = 0;
        sequence_letter_count = 0;
        quality_byte_count = 0;
    }

    /// Discards the unfinished record's batch tails and resets its encoder state.
    void rollback_current_record() {
        body.output_storage->resize(body.record_start_offset);
        header_storage->resize(header_start_offset);
        quality_storage->resize(quality_start_offset);
        reset_for_next_record();
    }

    /// Bind the parser state to the active batch's shared storage.
    void attach_to_batch(std::vector<char>& header_buf,
                         std::vector<p_storage_unit>& seq_buf,
                         std::vector<std::uint8_t>& quality_buf) noexcept {
        header_storage = &header_buf;
        body.output_storage = &seq_buf;
        quality_storage = &quality_buf;
    }
};

/// Continues one FASTQ record across input refills. Sequence and quality
/// fields may span several lines. Quality bytes are counted against the
/// input sequence length, so '@' and '+' within quality data stay unchanged.
///
/// On completion, `emit_record` receives `(header_start, header_size, seq_start,
/// seq_word_count, padding, qual_start, qual_size)` for the batch buffers.
/// `qual_size` is zero when retention is disabled; length validation still uses
/// the cumulative quality count for the entire record.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit = std::uint64_t,
          typename p_emit_record,
          typename p_failure>
[[nodiscard]] record_parse_result parse_one_fastq_record(
    fastq_parser_state<
        p_storage_unit,
        compact_sequence_range<
            sequence_custom_alphabet<p_alphabet, p_storage_unit>>::traits::bits_per_char,
        compact_sequence_range<
            sequence_custom_alphabet<p_alphabet, p_storage_unit>>::traits::letters_per_unit>& state,
    const std::uint8_t* slab_data,
    std::size_t slab_size,
    bool is_eof,
    std::size_t cursor,
    std::size_t global_offset,
    bool strict_fastq,
    p_emit_record&& emit_record,
    p_failure&& failure_fn) {

    using sequence_traits_type = typename compact_sequence_range<
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

    // Refill an unfinished record, or reject its truncated final input slab.
    const auto need_more = [&state, &parse_cursor, &failure_fn, is_eof, slab_size,
                            global_offset]() -> record_parse_result {
        if (is_eof) {
            parse_cursor = slab_size;
            state.rollback_current_record();
            emit_failure(failure_fn, "truncated FASTQ record at file position {}",
                         global_offset + parse_cursor);
            return record_parse_result{parse_outcome::abort, parse_cursor};
        }
        return record_parse_result{parse_outcome::incomplete, parse_cursor};
    };

    /// Completes only an exact quality count; EOF also rejects an incomplete one.
    const auto finish_quality = [&state, &parse_cursor, global_offset, &failure_fn](
                                    bool require_complete) -> parse_outcome {
        const std::size_t quality_size = state.quality_byte_count;
        const std::size_t sequence_size = state.sequence_letter_count;
        if (quality_size == sequence_size) {
            state.progress = fastq_parser_progress::finished_record;
            return parse_outcome::complete;
        }
        if (quality_size < sequence_size && !require_complete)
            return parse_outcome::incomplete;
        state.rollback_current_record();
        if (quality_size < sequence_size) {
            emit_failure(failure_fn,
                         "truncated FASTQ record (qual={}, seq={}) at file position {}",
                         quality_size, sequence_size, global_offset + parse_cursor);
        } else {
            emit_failure(failure_fn,
                         "FASTQ qual length mismatch (qual={}, seq={}) at file position {}",
                         quality_size, sequence_size, global_offset + parse_cursor);
        }
        return parse_outcome::abort;
    };

    // A FASTQ record starts with '@'.
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

    // Read the header through its newline.
    if (state.progress == fastq_parser_progress::in_header) {
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes) {
            state.header_storage->insert(
                state.header_storage->end(),
                reinterpret_cast<const char*>(slab_data + parse_cursor),
                reinterpret_cast<const char*>(slab_data + parse_cursor + remaining_bytes));
            parse_cursor = slab_size;
            return need_more();
        }
        state.header_storage->insert(
            state.header_storage->end(),
            reinterpret_cast<const char*>(slab_data + parse_cursor),
            reinterpret_cast<const char*>(slab_data + parse_cursor + newline_offset));
        // The terminating carriage return may have arrived in an earlier refill.
        if (state.header_storage->size() > state.header_start_offset &&
            state.header_storage->back() == '\r') {
            state.header_storage->pop_back();
        }
        parse_cursor += newline_offset + 1;
        state.progress = fastq_parser_progress::at_body_line_start;
    }

    // Read sequence lines until the separator.
    while (state.progress == fastq_parser_progress::at_body_line_start ||
           state.progress == fastq_parser_progress::in_sequence_line) {
        if (state.progress == fastq_parser_progress::at_body_line_start) {
            if (parse_cursor >= slab_size)
                return need_more();
            const std::uint8_t leading_byte = slab_data[parse_cursor];
            if (leading_byte == '+') {
                state.sequence_letter_count = state.sequence_byte_count;
                state.progress = fastq_parser_progress::at_separator_line;
                break;
            }
            if (leading_byte == '@' || leading_byte == '>') {
                state.sequence_letter_count = state.sequence_byte_count;
                if (strict_fastq) {
                    state.rollback_current_record();
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
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes) {
            return need_more();
        }
        std::size_t line_size = newline_offset;
        if (line_size > 0 && slab_data[parse_cursor + line_size - 1] == '\r') {
            line_size -= 1;
        }
        const auto encoded = bioseq::simd::active_target::encode_body_chunk<
            p_alphabet,
            bioseq::simd::active_target::unknown_byte_policy::reject,
            p_storage_unit,
            bits_per_char,
            letters_per_unit>(state.body, slab_data + parse_cursor, line_size);
        if (!encoded) {
            const auto failure_position = global_offset + parse_cursor + encoded.consumed_bytes;
            state.rollback_current_record();
            emit_failure(failure_fn, "invalid sequence byte 0x{:02x} at file position {}",
                static_cast<unsigned>(encoded.offending_byte), failure_position);
            return record_parse_result{parse_outcome::abort, parse_cursor + encoded.consumed_bytes};
        }
        state.sequence_byte_count += encoded.emitted_symbols;
        parse_cursor += newline_offset + 1;
        state.progress = fastq_parser_progress::at_body_line_start;
    }

    // Skip the separator line, including its optional comment.
    if (state.progress == fastq_parser_progress::at_separator_line) {
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes)
            return need_more();
        parse_cursor += newline_offset + 1;
        state.progress = fastq_parser_progress::in_quality_line;
    }

    // Newline and EOF quality lines share exact-length validation and rollback.
    while (state.progress == fastq_parser_progress::in_quality_line) {
        if (parse_cursor >= slab_size) {
            if (!is_eof)
                return need_more();
            if (finish_quality(true) == parse_outcome::abort)
                return record_parse_result{parse_outcome::abort, parse_cursor};
            break;
        }
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', slab_data + parse_cursor, remaining_bytes);
        const bool has_newline = newline_offset != remaining_bytes;
        if (!has_newline && !is_eof)
            return need_more();
        std::size_t line_size = newline_offset;
        if (line_size > 0u && slab_data[parse_cursor + line_size - 1u] == '\r')
            --line_size;
        const std::size_t next_cursor = parse_cursor + newline_offset + (has_newline ? 1u : 0u);
        const auto next_quality_count =
            bioseq::common::checked_add(state.quality_byte_count, line_size);
        if (!next_quality_count.has_value()) {
            parse_cursor = next_cursor;
            state.rollback_current_record();
            emit_failure(failure_fn, "FASTQ quality length overflow at file position {}",
                         global_offset + parse_cursor);
            return record_parse_result{parse_outcome::abort, parse_cursor};
        }
        if (state.retain_quality) {
            state.quality_storage->insert(state.quality_storage->end(),
                                          slab_data + parse_cursor,
                                          slab_data + parse_cursor + line_size);
        }
        state.quality_byte_count = *next_quality_count;
        parse_cursor = next_cursor;
        const auto quality_outcome = finish_quality(!has_newline);
        if (quality_outcome == parse_outcome::abort)
            return record_parse_result{parse_outcome::abort, parse_cursor};
        if (quality_outcome == parse_outcome::complete)
            break;
    }

    // Finish the packed sequence and emit the completed record.
    if (state.progress == fastq_parser_progress::finished_record) {
        const std::size_t header_size = state.header_storage->size() - state.header_start_offset;
        const std::size_t seq_start = state.body.record_start_offset;
        const auto [seq_word_count, padding] = bioseq::simd::active_target::
            finalize_encode_state<p_storage_unit, bits_per_char, letters_per_unit>(state.body);
        const std::size_t qual_size = state.quality_storage->size() - state.quality_start_offset;
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

} // namespace bioseq::io
