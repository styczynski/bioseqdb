module;

#include <hwy/highway.h>

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

export module bioseq_io.fastq;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.options;
import bioseq_io.errors;
import bioseq_io.encoder;
import bioseq_io.simd_scan;
import bioseq_io.fasta;  // reuse parse_outcome enum

export namespace bioseq::io {

/// FASTQ per-record FSM progress marker. Spec §10.1.
enum class fastq_progress : std::uint8_t {
    AT_HEADER_SIGIL = 0,
    IN_HEADER,
    AT_SEQ_LINE_START,
    AT_SEP_LINE,
    AT_QUAL_LINE_START,
    FINISHED_RECORD,
};

template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
struct fastq_parser_state {
    fastq_progress progress = fastq_progress::AT_HEADER_SIGIL;
    std::string header;
    std::vector<std::uint8_t> quality;          // raw Phred+33 bytes
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit> body{};
    std::size_t sequence_letter_count = 0;      // populated when seq line ends
    bool quality_started = false;

    void reset_for_next_record() noexcept {
        progress = fastq_progress::AT_HEADER_SIGIL;
        header.clear();
        quality.clear();
        body.reset();
        sequence_letter_count = 0;
        quality_started = false;
    }
};

/// Parse one FASTQ record. v1 contract: STRICT 4-line layout (header
/// line, sequence line, separator line, quality line). Multi-line FASTQ
/// is a phase-2 feature; the strict mode mirrors seq_io's approach
/// (deep-dive §3.3) and is the right default for high-throughput
/// short-read FASTQ.
///
/// `is_eof` allows the trailing record to be emitted even without a
/// final newline. Spec §10.
template <
    sequence_alphabet alphabet_t,
    typename storage_unit_t = std::uint64_t,
    typename emit_record_fn_t,
    typename failure_fn_t>
[[nodiscard]] parse_outcome parse_one_fastq_record(
    fastq_parser_state<storage_unit_t,
        compact_sequence_range<sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits::bits_per_char,
        compact_sequence_range<sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits::letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT slab_data,
    std::size_t slab_size,
    bool is_eof,
    std::size_t* cursor_inout,
    std::size_t global_offset,
    emit_record_fn_t&& emit_record,
    failure_fn_t&& failure_fn) {

    using sequence_traits =
        typename compact_sequence_range<
            sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits;
    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;

    namespace simd = bioseq::io::simd::HWY_NAMESPACE;
    namespace hn = hwy::HWY_NAMESPACE;
    constexpr hn::ScalableTag<std::uint8_t> byte_tag{};
    constexpr std::array<std::uint8_t, 1> needle_newline{'\n'};

    std::size_t cursor = *cursor_inout;

    auto need_more = [&]() -> parse_outcome {
        *cursor_inout = cursor;
        return parse_outcome::INCOMPLETE;
    };

    // Step 1: AT_HEADER_SIGIL — expect '@'.
    if (state.progress == fastq_progress::AT_HEADER_SIGIL) {
        if (cursor >= slab_size) {
            *cursor_inout = cursor;
            return is_eof ? parse_outcome::AT_EOF : parse_outcome::INCOMPLETE;
        }
        if (slab_data[cursor] != '@') {
            emit_failure(failure_fn,
                         "expected '@' at file position {}, got byte 0x{:02x}",
                         global_offset + cursor,
                         static_cast<unsigned>(slab_data[cursor]));
            return parse_outcome::ABORT;
        }
        cursor += 1;
        state.progress = fastq_progress::IN_HEADER;
    }

    // Step 2: IN_HEADER — scan to '\n'.
    if (state.progress == fastq_progress::IN_HEADER) {
        const std::size_t remaining = slab_size - cursor;
        const std::size_t nl_offset = simd::scan_for_first(
            byte_tag, needle_newline, slab_data + cursor, remaining);
        if (nl_offset == remaining) {
            state.header.append(reinterpret_cast<const char*>(slab_data + cursor),
                                remaining);
            cursor = slab_size;
            return need_more();
        }
        std::size_t header_size = nl_offset;
        if (header_size > 0 && slab_data[cursor + header_size - 1] == '\r') {
            header_size -= 1;
        }
        state.header.append(reinterpret_cast<const char*>(slab_data + cursor),
                            header_size);
        cursor += nl_offset + 1;
        state.progress = fastq_progress::AT_SEQ_LINE_START;
    }

    // Step 3: AT_SEQ_LINE_START — scan to '\n'; encode the body.
    if (state.progress == fastq_progress::AT_SEQ_LINE_START) {
        if (cursor >= slab_size) return need_more();
        const std::size_t remaining = slab_size - cursor;
        const std::size_t nl_offset = simd::scan_for_first(
            byte_tag, needle_newline, slab_data + cursor, remaining);
        if (nl_offset == remaining) {
            // Sequence line not terminated; ask for more.
            return need_more();
        }
        std::size_t line_size = nl_offset;
        if (line_size > 0 && slab_data[cursor + line_size - 1] == '\r') {
            line_size -= 1;
        }
        encode_body_chunk<alphabet_t,
                          storage_unit_t,
                          bits_per_char,
                          letters_per_unit>(
            state.body, slab_data + cursor, line_size);
        state.sequence_letter_count = state.body.total_letters;
        cursor += nl_offset + 1;
        state.progress = fastq_progress::AT_SEP_LINE;
    }

    // Step 4: AT_SEP_LINE — expect '+', then scan to '\n'.
    if (state.progress == fastq_progress::AT_SEP_LINE) {
        if (cursor >= slab_size) return need_more();
        if (slab_data[cursor] != '+') {
            emit_failure(failure_fn,
                         "expected '+' separator at file position {}, got byte 0x{:02x}",
                         global_offset + cursor,
                         static_cast<unsigned>(slab_data[cursor]));
            return parse_outcome::ABORT;
        }
        const std::size_t remaining = slab_size - cursor;
        const std::size_t nl_offset = simd::scan_for_first(
            byte_tag, needle_newline, slab_data + cursor, remaining);
        if (nl_offset == remaining) return need_more();
        cursor += nl_offset + 1;
        state.progress = fastq_progress::AT_QUAL_LINE_START;
    }

    // Step 5: AT_QUAL_LINE_START — scan to '\n', strip '\r', verify
    // length == sequence length. v1: single-line qual only.
    if (state.progress == fastq_progress::AT_QUAL_LINE_START) {
        if (cursor >= slab_size) return need_more();
        const std::size_t remaining = slab_size - cursor;
        const std::size_t nl_offset = simd::scan_for_first(
            byte_tag, needle_newline, slab_data + cursor, remaining);
        if (nl_offset == remaining) {
            if (is_eof) {
                std::size_t line_size = remaining;
                if (line_size > 0 && slab_data[cursor + line_size - 1] == '\r') {
                    line_size -= 1;
                }
                state.quality.insert(state.quality.end(),
                                     slab_data + cursor,
                                     slab_data + cursor + line_size);
                cursor += remaining;
                state.progress = fastq_progress::FINISHED_RECORD;
            } else {
                return need_more();
            }
        } else {
            std::size_t line_size = nl_offset;
            if (line_size > 0 && slab_data[cursor + line_size - 1] == '\r') {
                line_size -= 1;
            }
            state.quality.insert(state.quality.end(),
                                 slab_data + cursor,
                                 slab_data + cursor + line_size);
            cursor += nl_offset + 1;
            state.progress = fastq_progress::FINISHED_RECORD;
        }
    }

    if (state.progress == fastq_progress::FINISHED_RECORD) {
        if (state.quality.size() != state.sequence_letter_count) {
            emit_failure(failure_fn,
                         "FASTQ qual length mismatch (qual={}, seq={}) at file position {}",
                         state.quality.size(),
                         state.sequence_letter_count,
                         global_offset + cursor);
            return parse_outcome::ABORT;
        }
        auto finalised = finalize_encode_state<storage_unit_t,
                                               bits_per_char,
                                               letters_per_unit>(state.body);
        emit_record(std::move(state.header),
                    std::move(finalised.first),
                    finalised.second,
                    std::move(state.quality));
        state.reset_for_next_record();
        *cursor_inout = cursor;
        return parse_outcome::COMPLETE;
    }

    return need_more();
}

}  // namespace bioseq::io
