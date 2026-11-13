/**
 * @file fastq.cppm
 * @brief Incremental FASTQ record parser.
 *
 * Multi-line sequence fields and requested quality fields are appended directly
 * to batch-owned storage. Quality completion is determined by byte count, so
 * marker characters at the start of a quality line remain data.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <libassert/assert.hpp>
#include <string>
#include <span>
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

/// Structural framing shares record/line/count rules without allocating decoded fields.
/// Alphabet and printable-quality validation remain the subsequent decoder's responsibility.
enum class fastq_parse_mode : std::uint8_t { decode, frame };

/// State for one FASTQ record, aligned to keep concurrent parser states apart.
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
    bool sequence_pending_cr = false;
    /// Expected quality length saved when sequence parsing ends.
    std::size_t sequence_letter_count = 0;
    /// Quality bytes excluding line endings, independent of retained storage.
    std::size_t quality_byte_count = 0;
    /// Retain batch quality bytes; sequence-only streams disable this before parsing.
    /// This policy survives record resets and storage reattachment.
    bool retain_quality = true;
    /// Optional bounded quality delivery. Retention remains independent for batch readers.
    void* quality_sink_state = nullptr;
    bool (*quality_sink)(void*, std::span<const std::uint8_t>, std::size_t) = nullptr;
    bool quality_pending_cr = false;
    /// Borrowed pointers to the batch's shared storage.
    std::vector<char>* header_storage = nullptr;
    raw_header_buffer* streamed_header = nullptr;

    /** Count the selected batch or admitted streaming header owner. */
    [[nodiscard]] std::size_t header_size() const noexcept {
        return streamed_header ? streamed_header->size() : header_storage->size();
    }
    /** Borrow a completed header for bounded diagnostics. */
    [[nodiscard]] std::string_view header_view() const noexcept {
        return streamed_header ? streamed_header->view()
            : std::string_view{header_storage->data() + header_start_offset, header_storage->size() - header_start_offset};
    }
    /** Append raw bytes and normalize a split final carriage return once. */
    [[nodiscard]] std::expected<void, std::string> append_header(std::string_view bytes, bool complete) {
        if (streamed_header) return streamed_header->append_fragment(bytes, complete);
        header_storage->insert(header_storage->end(), bytes.begin(), bytes.end());
        if (complete && header_storage->size() > header_start_offset && header_storage->back() == '\r')
            header_storage->pop_back();
        return {};
    }
    std::vector<std::uint8_t>* quality_storage = nullptr;

    /// Reset record-local progress while keeping attached batch storage.
    void reset_for_next_record() noexcept {
        progress = fastq_parser_progress::at_header_sigil;
        body.reset();
        sequence_byte_count = 0;
        sequence_pending_cr = false;
        sequence_letter_count = 0;
        quality_byte_count = 0;
        quality_pending_cr = false;
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
/// the cumulative quality count for the entire record. Frame mode emits zero
/// storage extents and retains only parser counters; it establishes complete
/// record boundaries before a parallel decoder performs alphabet/quality checks.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit = std::uint64_t,
          fastq_parse_mode Mode = fastq_parse_mode::decode,
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

    const auto reject_quality_byte = [&state, &failure_fn](unsigned byte, std::size_t field_offset,
                                         std::size_t failure_cursor,
                                         std::size_t file_position) -> record_parse_result {
        std::string header;
        if constexpr (Mode == fastq_parse_mode::decode)
            header.assign(state.header_view().substr(0u, 256u));
        state.rollback_current_record();
        emit_failure(failure_fn, input_failure_kind::invalid_quality_byte,
            "invalid FASTQ quality byte {} at quality offset {} in record '{}' (file position {})",
            byte, field_offset, header, file_position);
        return {parse_outcome::abort, failure_cursor};
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
        state.header_start_offset = state.header_size();
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
        if constexpr (Mode == fastq_parse_mode::decode) {
            const auto appended = state.append_header(
                {reinterpret_cast<const char*>(slab_data + parse_cursor), newline_offset}, newline_offset != remaining_bytes);
            if (!appended) {
                emit_failure(failure_fn, "{}", appended.error());
                return record_parse_result{parse_outcome::abort, parse_cursor};
            }
        }
        if (newline_offset == remaining_bytes) {
            parse_cursor = slab_size;
            return need_more();
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

        if (state.sequence_pending_cr) {
            if (parse_cursor == slab_size) return need_more();
            if (slab_data[parse_cursor] != '\n') {
                state.rollback_current_record();
                emit_failure(failure_fn, "standalone CR in FASTQ sequence at file position {}",
                             global_offset + parse_cursor);
                return {parse_outcome::abort, parse_cursor};
            }
            state.sequence_pending_cr = false;
        }
        if (parse_cursor == slab_size) return need_more();
        // Consume each refill, even when an unwrapped sequence line is larger
        // than the input slab. Only the encoder's partial word spans refills.
        const auto remaining_bytes = std::min<std::size_t>(slab_size - parse_cursor, 65536);
        // Stop at either line ending in one pass. A separate CR scan would
        // repeatedly traverse later records in ordinary LF-only input slabs.
        const auto line_size = simd::scan_first_byte_pair('\n', '\r',
            slab_data + parse_cursor, remaining_bytes);
        if (line_size == 0) {
            if (slab_data[parse_cursor++] == '\r') state.sequence_pending_cr = true;
            else state.progress = fastq_parser_progress::at_body_line_start;
            continue;
        }
        std::size_t emitted_symbols = line_size;
        if constexpr (Mode == fastq_parse_mode::decode) {
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
            emitted_symbols = encoded.emitted_symbols;
        }
        const auto next_count = bioseq::common::checked_add(state.sequence_byte_count, emitted_symbols);
        if (!next_count) {
            state.rollback_current_record();
            emit_failure(failure_fn, "FASTQ sequence length overflow at file position {}",
                         global_offset + parse_cursor);
            return {parse_outcome::abort, parse_cursor};
        }
        state.sequence_byte_count = *next_count;
        parse_cursor += line_size;
    }

    // Skip the separator line, including its optional comment.
    if (state.progress == fastq_parser_progress::at_separator_line) {
        const std::size_t remaining_bytes = slab_size - parse_cursor;
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', slab_data + parse_cursor, remaining_bytes);
        if (newline_offset == remaining_bytes) {
            // The ignored comment needs no retained bytes between refills.
            parse_cursor = slab_size;
            return need_more();
        }
        parse_cursor += newline_offset + 1;
        state.progress = fastq_parser_progress::in_quality_line;
    }

    // Newline and EOF quality lines share exact-length validation and rollback.
    while (state.progress == fastq_parser_progress::in_quality_line) {
        if (state.quality_pending_cr) {
            if (parse_cursor == slab_size && !is_eof) return need_more();
            if (parse_cursor == slab_size || slab_data[parse_cursor] != '\n') {
                return reject_quality_byte('\r', state.quality_byte_count, parse_cursor,
                    global_offset + parse_cursor - 1);
            }
            state.quality_pending_cr = false;
        }
        if (parse_cursor >= slab_size) {
            if (!is_eof)
                return need_more();
            if (finish_quality(true) == parse_outcome::abort)
                return record_parse_result{parse_outcome::abort, parse_cursor};
            break;
        }
        // Consume bounded spans even when one quality line exceeds the refill buffer.
        const auto remaining_bytes = std::min<std::size_t>(slab_size - parse_cursor, 65536);
        std::size_t line_size{};
        if constexpr (Mode == fastq_parse_mode::frame) {
            line_size = simd::scan_first_byte_pair('\n', '\r', slab_data + parse_cursor, remaining_bytes);
        } else if (strict_fastq) {
            // Newlines and CR are outside the accepted quality interval, so
            // this one SIMD pass finds both the line boundary and bad bytes.
            line_size = simd::scan_fastq_quality_prefix(slab_data + parse_cursor, remaining_bytes);
            if (line_size != remaining_bytes) {
                const auto byte = slab_data[parse_cursor + line_size];
                if (byte != '\n' && byte != '\r')
                    return reject_quality_byte(byte, state.quality_byte_count + line_size,
                        parse_cursor + line_size, global_offset + parse_cursor + line_size);
            }
        } else {
            line_size = simd::scan_first_byte_pair('\n', '\r', slab_data + parse_cursor, remaining_bytes);
        }
        const std::size_t next_cursor = parse_cursor + line_size;
        const auto next_quality_count =
            bioseq::common::checked_add(state.quality_byte_count, line_size);
        if (!next_quality_count.has_value()) {
            parse_cursor = next_cursor;
            state.rollback_current_record();
            emit_failure(failure_fn, "FASTQ quality length overflow at file position {}",
                         global_offset + parse_cursor);
            return record_parse_result{parse_outcome::abort, parse_cursor};
        }
        if (*next_quality_count > state.sequence_letter_count) {
            state.rollback_current_record();
            emit_failure(failure_fn, "FASTQ quality exceeds sequence length at file position {}", global_offset + next_cursor);
            return {parse_outcome::abort, next_cursor};
        }
        if constexpr (Mode == fastq_parse_mode::decode) {
            if (line_size && state.quality_sink && !state.quality_sink(state.quality_sink_state,
                    {slab_data + parse_cursor, line_size}, state.quality_byte_count)) {
                state.rollback_current_record();
                emit_failure(failure_fn, "FASTQ quality sink rejected input at file position {}", global_offset + parse_cursor);
                return {parse_outcome::abort, parse_cursor};
            }
            if (state.retain_quality) {
                state.quality_storage->insert(state.quality_storage->end(),
                                              slab_data + parse_cursor,
                                              slab_data + parse_cursor + line_size);
            }
        }
        state.quality_byte_count = *next_quality_count;
        parse_cursor = next_cursor;
        if (parse_cursor < slab_size && slab_data[parse_cursor] == '\r') {
            state.quality_pending_cr = true;
            ++parse_cursor;
            continue;
        }
        const bool has_newline = parse_cursor < slab_size && slab_data[parse_cursor] == '\n';
        if (has_newline) ++parse_cursor;
        else if (!is_eof || parse_cursor < slab_size) continue;
        const auto quality_outcome = finish_quality(!has_newline);
        if (quality_outcome == parse_outcome::abort)
            return record_parse_result{parse_outcome::abort, parse_cursor};
        if (quality_outcome == parse_outcome::complete)
            break;
    }

    // Finish the packed sequence and emit the completed record.
    if (state.progress == fastq_parser_progress::finished_record) {
        const std::size_t header_size = state.header_size() - state.header_start_offset;
        const std::size_t seq_start = state.body.record_start_offset;
        std::size_t seq_word_count{}, padding{};
        if constexpr (Mode == fastq_parse_mode::decode) {
            const auto finished = bioseq::simd::active_target::
                finalize_encode_state<p_storage_unit, bits_per_char, letters_per_unit>(state.body);
            seq_word_count = finished.first;
            padding = finished.second;
        }
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
