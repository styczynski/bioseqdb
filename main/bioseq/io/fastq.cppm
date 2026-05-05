/**
 * @file fastq.cppm
 * @brief Per-record FASTQ FSM driver. kseqpp-port behaviour with
 *        Highway-backed `\n` scans hidden behind the
 *        `simd::scan_first_byte` non-inline wrapper.
 *
 * v2 storage model: header bytes flow directly into the batch's
 * shared `_header_storage`, sequence words into `_seq_storage`,
 * quality bytes into `_quality_storage`. The emit-record callback
 * only records the per-record offset / padding tuple. Per-record
 * heap allocation count: 0.
 *
 * The state machine is the textbook FASTQ shape extended for the two
 * subtleties spec §10 calls out:
 *
 *   1. **Multi-line bodies.** Both the sequence and the quality may
 *      span multiple `\n`-separated lines. The body loop re-enters
 *      `AT_SEQ_LINE_START` for each non-`+` line until it sees a `+`.
 *      The qual loop accumulates until
 *      `quality.size() == sequence_letters`.
 *
 *   2. **Byte-count terminator on quality.** Quality lines may legally
 *      start with `@`, `+`, or `>`. We MUST NOT use a sentinel-based
 *      terminator on qual; instead we count stripped-CR bytes.
 *
 * `strict_fastq` (default true) controls how the FSM reacts to a
 * missing or truncated qual section. Spec §10.1 / §10.2 / §10.3.
 *
 * Highway is included directly so the FSM can call
 * `bioseq::simd::N_NEON::encode_body_chunk*` without a
 * translation layer (mirrors `fasta.cppm`).
 */
module;

#include <hwy/highway.h>

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

export module bioseq_io.fastq;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_io.options;
import bioseq_io.errors;
import bioseq_io.simd_kernels;
import bioseq_io.fasta;  // reuse parse_outcome enum

export namespace bioseq::io {

/// FASTQ per-record FSM progress marker. Spec §10.1.
enum class fastq_progress : std::uint8_t {
    AT_HEADER_SIGIL = 0,    ///< Expect '@'.
    IN_HEADER,              ///< Accumulating header bytes.
    AT_BODY_LINE_START,     ///< Peeking the first byte of the next body line.
    IN_SEQ_LINE,            ///< Mid-sequence-line scan to next '\n'.
    AT_SEP_LINE,            ///< Just consumed '+', skipping comment to '\n'.
    IN_QUAL_LINE,           ///< Mid-quality-line scan; accumulating bytes.
    FINISHED_RECORD,        ///< Ready for emit.
};

/// Per-record FASTQ parser state. Aligned to a cache line (spec §13.4)
/// so a future multi-parser pipeline can store one state per parser
/// thread without false-sharing the partial-encoding fields.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
struct alignas(64) fastq_parser_state {
    fastq_progress progress = fastq_progress::AT_HEADER_SIGIL;
    std::size_t header_start_offset = 0;
    std::size_t quality_start_offset = 0;
    bioseq::simd::N_NEON::encode_state<storage_unit_t, BitsPerChar, LettersPerUnit> body{};
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
        progress = fastq_progress::AT_HEADER_SIGIL;
        body.reset();
        sequence_byte_count = 0;
        sequence_letter_count = 0;
    }

    void attach_to_batch(std::vector<char>& header_buf,
                         std::vector<storage_unit_t>& seq_buf,
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
    sequence_alphabet alphabet_t,
    typename storage_unit_t = std::uint64_t,
    typename emit_record_fn_t,
    typename failure_fn_t>
[[nodiscard]] parse_step_result parse_one_fastq_record(
    fastq_parser_state<storage_unit_t,
        compact_sequence_range<sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits::bits_per_char,
        compact_sequence_range<sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits::letters_per_unit>& state,
    const std::uint8_t* slab_data,
    std::size_t slab_size,
    bool is_eof,
    std::size_t cursor,
    std::size_t global_offset,
    bool strict_fastq,
    emit_record_fn_t&& emit_record,
    failure_fn_t&& failure_fn) {

    using sequence_traits =
        typename compact_sequence_range<
            sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits;
    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;

    DEBUG_ASSERT(state.header_storage != nullptr,
                 "fastq_parser_state must be attached to a batch via attach_to_batch");
    DEBUG_ASSERT(state.body.output_storage != nullptr,
                 "fastq_parser_state must be attached to a batch via attach_to_batch");
    DEBUG_ASSERT(state.quality_storage != nullptr,
                 "fastq_parser_state must be attached to a batch via attach_to_batch");

    std::size_t parse_cursor = cursor;

    auto need_more = [&]() -> parse_step_result {
        return parse_step_result{parse_outcome::INCOMPLETE, parse_cursor};
    };

    // ----- Step 1: AT_HEADER_SIGIL — expect '@' -----
    if (state.progress == fastq_progress::AT_HEADER_SIGIL) {
        if (parse_cursor >= slab_size) {
            return parse_step_result{
                is_eof ? parse_outcome::AT_EOF : parse_outcome::INCOMPLETE,
                parse_cursor};
        }
        if (slab_data[parse_cursor] != '@') {
            emit_failure(failure_fn,
                         "expected '@' at file position {}, got byte 0x{:02x}",
                         global_offset + parse_cursor,
                         static_cast<unsigned>(slab_data[parse_cursor]));
            return parse_step_result{parse_outcome::ABORT, parse_cursor};
        }
        // Snapshot record-start offsets for emit_record.
        state.header_start_offset = state.header_storage->size();
        state.body.record_start_offset = state.body.output_storage->size();
        state.quality_start_offset = state.quality_storage->size();
        parse_cursor += 1;
        state.progress = fastq_progress::IN_HEADER;
    }

    // ----- Step 2: IN_HEADER — scan to '\n' -----
    if (state.progress == fastq_progress::IN_HEADER) {
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
        state.progress = fastq_progress::AT_BODY_LINE_START;
    }

    // ----- Step 3: body lines — accumulate seq lines until '+' -----
    while (state.progress == fastq_progress::AT_BODY_LINE_START ||
           state.progress == fastq_progress::IN_SEQ_LINE) {
        if (state.progress == fastq_progress::AT_BODY_LINE_START) {
            if (parse_cursor >= slab_size) return need_more();
            const std::uint8_t leading_byte = slab_data[parse_cursor];
            if (leading_byte == '+') {
                state.sequence_letter_count = state.sequence_byte_count;
                state.progress = fastq_progress::AT_SEP_LINE;
                break;
            }
            if (leading_byte == '@' || leading_byte == '>') {
                state.sequence_letter_count = state.sequence_byte_count;
                if (strict_fastq) {
                    emit_failure(failure_fn,
                                 "FASTQ record without quality at file position {}",
                                 global_offset + parse_cursor);
                    return parse_step_result{parse_outcome::ABORT, parse_cursor};
                }
                state.progress = fastq_progress::FINISHED_RECORD;
                break;
            }
            state.progress = fastq_progress::IN_SEQ_LINE;
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
        bioseq::simd::N_NEON::encode_body_chunk<
            alphabet_t,
            bioseq::simd::N_NEON::unknown_byte_policy::DROP,
            storage_unit_t, bits_per_char, letters_per_unit>(
                state.body, slab_data + parse_cursor, line_size);
        state.sequence_byte_count += line_size;
        parse_cursor += newline_offset + 1;
        state.progress = fastq_progress::AT_BODY_LINE_START;
    }

    // ----- Step 4: AT_SEP_LINE — consume '+...\n' -----
    if (state.progress == fastq_progress::AT_SEP_LINE) {
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes) return need_more();
        parse_cursor += newline_offset + 1;
        state.progress = fastq_progress::IN_QUAL_LINE;
    }

    // ----- Step 5: IN_QUAL_LINE — multi-line qual accumulation -----
    while (state.progress == fastq_progress::IN_QUAL_LINE) {
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
                    return parse_step_result{parse_outcome::ABORT, parse_cursor};
                }
                state.progress = fastq_progress::FINISHED_RECORD;
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
                    return parse_step_result{parse_outcome::ABORT, parse_cursor};
                }
                state.progress = fastq_progress::FINISHED_RECORD;
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
                return parse_step_result{parse_outcome::ABORT, parse_cursor};
            }
            state.progress = fastq_progress::FINISHED_RECORD;
            break;
        }
    }

    // ----- Step 6: FINISHED_RECORD — finalise + emit -----
    if (state.progress == fastq_progress::FINISHED_RECORD) {
        const std::size_t header_size =
            state.header_storage->size() - state.header_start_offset;
        const std::size_t seq_start = state.body.record_start_offset;
        const auto [seq_word_count, padding] =
            bioseq::simd::N_NEON::finalize_encode_state<storage_unit_t,
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
        return parse_step_result{parse_outcome::COMPLETE, parse_cursor};
    }

    return need_more();
}

}  // namespace bioseq::io
