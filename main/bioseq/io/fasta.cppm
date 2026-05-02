module;

#include <hwy/highway.h>

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

export module bioseq_io.fasta;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.options;
import bioseq_io.errors;
import bioseq_io.encoder;
import bioseq_io.simd_scan;

export namespace bioseq::io {

/// FASTA per-record FSM progress marker. Used to resume parsing after a
/// refill. Spec §7.4.
enum class fasta_progress : std::uint8_t {
    AT_HEADER_SIGIL = 0,
    IN_HEADER,
    AT_BODY_LINE_START,
    FINISHED_RECORD,
};

/// Outcome of a single `parse_one_fasta_record` call.
enum class parse_outcome : std::uint8_t {
    COMPLETE = 0,    ///< A record was emitted; cursor advanced past it.
    INCOMPLETE,      ///< Need more bytes; refill and call again.
    AT_EOF,          ///< Slab is empty and EOF — no more records.
    ABORT,           ///< failure_fn invoked; abort the pipeline.
};

/// Pulled out so the parser state can also be created standalone.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
struct fasta_parser_state {
    fasta_progress progress = fasta_progress::AT_HEADER_SIGIL;
    std::string header;  // accumulated across refills
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit> body{};

    void reset_for_next_record() noexcept {
        progress = fasta_progress::AT_HEADER_SIGIL;
        header.clear();
        body.reset();
    }
};

/// Parse one FASTA record from the slab, given the state. On COMPLETE,
/// the caller appends the record to its batch and recycles the state.
/// `slab_data` / `slab_size` are the readable region; the parse cursor
/// is `*cursor_inout`. Spec §9.2.
///
/// `is_eof` is the producer's "no more bytes coming" flag for this
/// slab; lets us emit the trailing record cleanly.
template <
    sequence_alphabet alphabet_t,
    typename storage_unit_t = std::uint64_t,
    typename emit_record_fn_t,
    typename failure_fn_t>
[[nodiscard]] parse_outcome parse_one_fasta_record(
    fasta_parser_state<storage_unit_t,
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

    std::size_t cursor = *cursor_inout;
    constexpr std::array<std::uint8_t, 1> needle_newline{'\n'};

    // Step 1: AT_HEADER_SIGIL — expect '>' or EOF.
    if (state.progress == fasta_progress::AT_HEADER_SIGIL) {
        if (cursor >= slab_size) {
            *cursor_inout = cursor;
            return is_eof ? parse_outcome::AT_EOF : parse_outcome::INCOMPLETE;
        }
        if (slab_data[cursor] != '>') {
            emit_failure(failure_fn,
                         "expected '>' at file position {}, got byte 0x{:02x}",
                         global_offset + cursor,
                         static_cast<unsigned>(slab_data[cursor]));
            return parse_outcome::ABORT;
        }
        cursor += 1;  // consume '>'
        state.progress = fasta_progress::IN_HEADER;
    }

    // Step 2: IN_HEADER — scan to '\n', strip optional '\r'.
    if (state.progress == fasta_progress::IN_HEADER) {
        const std::size_t remaining = slab_size - cursor;
        const std::size_t nl_offset = simd::scan_for_first(
            byte_tag, needle_newline, slab_data + cursor, remaining);
        if (nl_offset == remaining) {
            // Header line did not terminate inside slab. Buffer what we
            // have and request a refill.
            state.header.append(reinterpret_cast<const char*>(slab_data + cursor),
                                remaining);
            cursor = slab_size;
            *cursor_inout = cursor;
            return parse_outcome::INCOMPLETE;
        }
        std::size_t header_size = nl_offset;
        if (header_size > 0 && slab_data[cursor + header_size - 1] == '\r') {
            header_size -= 1;
        }
        state.header.append(reinterpret_cast<const char*>(slab_data + cursor),
                            header_size);
        cursor += nl_offset + 1;  // consume '\n'
        state.progress = fasta_progress::AT_BODY_LINE_START;
    }

    // Step 3 & 4: body lines, looking for the next '>' that terminates this record.
    while (state.progress == fasta_progress::AT_BODY_LINE_START) {
        if (cursor >= slab_size) {
            if (is_eof) {
                state.progress = fasta_progress::FINISHED_RECORD;
                break;
            }
            *cursor_inout = cursor;
            return parse_outcome::INCOMPLETE;
        }
        if (slab_data[cursor] == '>') {
            state.progress = fasta_progress::FINISHED_RECORD;
            break;
        }
        const std::size_t remaining = slab_size - cursor;
        const std::size_t nl_offset = simd::scan_for_first(
            byte_tag, needle_newline, slab_data + cursor, remaining);
        if (nl_offset == remaining) {
            // Body line did not terminate inside slab. Two cases:
            //   1. is_eof: take the partial line as the final body line.
            //   2. !is_eof: refill needed; request INCOMPLETE without
            //      encoding the partial line (the caller's make_room
            //      will keep it in the slab so the next call sees it
            //      whole).
            if (is_eof) {
                std::size_t line_size = remaining;
                if (line_size > 0 && slab_data[cursor + line_size - 1] == '\r') {
                    line_size -= 1;
                }
                encode_body_chunk<alphabet_t,
                                  storage_unit_t,
                                  bits_per_char,
                                  letters_per_unit>(
                    state.body, slab_data + cursor, line_size);
                cursor += remaining;
                state.progress = fasta_progress::FINISHED_RECORD;
                break;
            }
            *cursor_inout = cursor;
            return parse_outcome::INCOMPLETE;
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
        cursor += nl_offset + 1;  // consume '\n'
        // Loop back to AT_BODY_LINE_START.
    }

    // Step 5: FINISHED_RECORD — finalise, emit, reset.
    if (state.progress == fasta_progress::FINISHED_RECORD) {
        auto finalised = finalize_encode_state<storage_unit_t,
                                               bits_per_char,
                                               letters_per_unit>(state.body);
        emit_record(std::move(state.header),
                    std::move(finalised.first),
                    finalised.second);
        state.reset_for_next_record();
        *cursor_inout = cursor;
        return parse_outcome::COMPLETE;
    }

    *cursor_inout = cursor;
    return parse_outcome::INCOMPLETE;
}

}  // namespace bioseq::io
