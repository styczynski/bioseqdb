/**
 * @file fasta.cppm
 * @brief Per-record FASTA FSM driver.
 *
 * Walks the slab via `bioseq_io.simd_kernels::scan_first_byte` to find
 * record boundaries. The encoder pipeline lives in `bioseq_io.encoder`
 * + `bioseq_io.encoder_dispatch`; this module is the FSM glue.
 *
 * v2 storage model: the FSM appends header bytes directly into the
 * batch's shared `_header_storage`, and the encoder pushes packed
 * sequence words directly into the batch's shared `_seq_storage`. The
 * emit-record callback only records the per-record offset / padding
 * triple. There is NO per-record `std::string` or `std::vector`
 * allocation on this path — the batch's three backing vectors grow
 * geometrically across thousands of records.
 *
 * No `<hwy/highway.h>` here — the Highway primitives the FSM needs are
 * exposed as non-inline wrappers from `simd_kernels` so this module's
 * BMI doesn't pull Highway's per-target inline operator overloads
 * (which would otherwise trigger a "definition with same mangled
 * name" collision when a downstream TU imports two Highway-aware
 * BMIs). CODE_STYLE.md §2 requires Highway code to live inside
 * `HWY_BEFORE_NAMESPACE()` brackets — by routing through the
 * non-inline wrappers, the FASTA FSM stays Highway-free at the
 * source level.
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq_io.fasta;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.options;
import bioseq_io.errors;
import bioseq_io.encoder;
import bioseq_io.encoder_dispatch;
import bioseq_io.simd_kernels;

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

/// Returned by `parse_one_fasta_record` / `parse_one_fastq_record` to
/// avoid an out-pointer for the cursor (CODE_STYLE.md §1 / §5
/// forbids output pointer parameters). The caller does
/// `auto step = parse_one_*_record(...); slab.parse_cursor =
/// step.cursor;` after each call.
struct parse_step_result {
    parse_outcome outcome = parse_outcome::INCOMPLETE;
    std::size_t   cursor  = 0;
};

/// Per-record FASTA parser state. Aligned to a cache line so a future
/// multi-parser pipeline (spec §13.4) can store one state per parser
/// thread without false-sharing the partial-encoding fields.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
struct alignas(64) fasta_parser_state {
    fasta_progress progress = fasta_progress::AT_HEADER_SIGIL;
    /// Offset into the batch's `_header_storage` where the current
    /// record's header bytes start. Captured at the AT_HEADER_SIGIL →
    /// IN_HEADER transition. Used by the emit_record callback.
    std::size_t header_start_offset = 0;
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit> body{};
    /// Borrowed pointer to the batch's shared header buffer. The FSM
    /// pushes header bytes into this directly to avoid the v1
    /// per-record `std::string` allocation.
    std::vector<char>* header_storage = nullptr;

    void reset_for_next_record() noexcept {
        progress = fasta_progress::AT_HEADER_SIGIL;
        body.reset();
    }

    /// Called by the FSM driver to point this state at the active
    /// batch's storage. Re-bound at the top of every parse iteration
    /// so a `make_room` slide on the producer side that triggers a
    /// realloc on the batch's vector is observed before the next
    /// emit-record.
    void attach_to_batch(std::vector<char>& header_buf,
                         std::vector<storage_unit_t>& seq_buf) noexcept {
        header_storage = &header_buf;
        body.output_storage = &seq_buf;
    }
};

/// Parse one FASTA record from the slab, given the state. On COMPLETE,
/// the caller appends the record to its batch and recycles the state.
/// `slab_data` / `slab_size` are the readable region; the parse cursor
/// is `*cursor_inout`. Spec §9.2.
///
/// `is_eof` is the producer's "no more bytes coming" flag for this
/// slab; lets us emit the trailing record cleanly.
///
/// `emit_record` is invoked with
/// `(header_start_offset, header_size, seq_start_offset, seq_word_count, padding)`
/// — offsets into the batch's shared storage, plus the encoder's
/// `(word_count, padding)` summary.
template <
    sequence_alphabet alphabet_t,
    typename storage_unit_t = std::uint64_t,
    typename emit_record_fn_t,
    typename failure_fn_t>
[[nodiscard]] parse_step_result parse_one_fasta_record(
    fasta_parser_state<storage_unit_t,
        compact_sequence_range<sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits::bits_per_char,
        compact_sequence_range<sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits::letters_per_unit>& state,
    const std::uint8_t* slab_data,
    std::size_t slab_size,
    bool is_eof,
    std::size_t cursor,
    std::size_t global_offset,
    emit_record_fn_t&& emit_record,
    failure_fn_t&& failure_fn) {

    using sequence_traits =
        typename compact_sequence_range<
            sequence_custom_alphabet<alphabet_t, storage_unit_t>>::traits;
    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;

    DEBUG_ASSERT(state.header_storage != nullptr,
                 "fasta_parser_state must be attached to a batch via attach_to_batch");
    DEBUG_ASSERT(state.body.output_storage != nullptr,
                 "fasta_parser_state must be attached to a batch via attach_to_batch");

    // Step 1: AT_HEADER_SIGIL — expect '>' or EOF.
    if (state.progress == fasta_progress::AT_HEADER_SIGIL) {
        if (cursor >= slab_size) {
            return parse_step_result{
                is_eof ? parse_outcome::AT_EOF : parse_outcome::INCOMPLETE,
                cursor};
        }
        if (slab_data[cursor] != '>') {
            emit_failure(failure_fn,
                         "expected '>' at file position {}, got byte 0x{:02x}",
                         global_offset + cursor,
                         static_cast<unsigned>(slab_data[cursor]));
            return parse_step_result{parse_outcome::ABORT, cursor};
        }
        // Snapshot offsets at the start of the record for emit_record.
        state.header_start_offset = state.header_storage->size();
        state.body.record_start_offset = state.body.output_storage->size();
        cursor += 1;  // consume '>'
        state.progress = fasta_progress::IN_HEADER;
    }

    // Step 2: IN_HEADER — scan to '\n', strip optional '\r'.
    if (state.progress == fasta_progress::IN_HEADER) {
        const std::size_t remaining = slab_size - cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + cursor, remaining);
        if (newline_offset == remaining) {
            state.header_storage->insert(
                state.header_storage->end(),
                reinterpret_cast<const char*>(slab_data + cursor),
                reinterpret_cast<const char*>(slab_data + cursor + remaining));
            return parse_step_result{parse_outcome::INCOMPLETE, slab_size};
        }
        std::size_t header_size = newline_offset;
        if (header_size > 0 && slab_data[cursor + header_size - 1] == '\r') {
            header_size -= 1;
        }
        state.header_storage->insert(
            state.header_storage->end(),
            reinterpret_cast<const char*>(slab_data + cursor),
            reinterpret_cast<const char*>(slab_data + cursor + header_size));
        cursor += newline_offset + 1;  // consume '\n'
        state.progress = fasta_progress::AT_BODY_LINE_START;
    }

    // Step 3 & 4: body lines, looking for the next '>' that terminates this record.
    while (state.progress == fasta_progress::AT_BODY_LINE_START) {
        if (cursor >= slab_size) {
            if (is_eof) {
                state.progress = fasta_progress::FINISHED_RECORD;
                break;
            }
            return parse_step_result{parse_outcome::INCOMPLETE, cursor};
        }
        if (slab_data[cursor] == '>') {
            state.progress = fasta_progress::FINISHED_RECORD;
            break;
        }
        const std::size_t remaining = slab_size - cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + cursor, remaining);
        if (newline_offset == remaining) {
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
            return parse_step_result{parse_outcome::INCOMPLETE, cursor};
        }
        std::size_t line_size = newline_offset;
        if (line_size > 0 && slab_data[cursor + line_size - 1] == '\r') {
            line_size -= 1;
        }
        encode_body_chunk<alphabet_t,
                          storage_unit_t,
                          bits_per_char,
                          letters_per_unit>(
            state.body, slab_data + cursor, line_size);
        cursor += newline_offset + 1;  // consume '\n'
        // Loop back to AT_BODY_LINE_START.
    }

    // Step 5: FINISHED_RECORD — finalise, emit, reset.
    if (state.progress == fasta_progress::FINISHED_RECORD) {
        const std::size_t header_size =
            state.header_storage->size() - state.header_start_offset;
        const std::size_t seq_start = state.body.record_start_offset;
        const auto [seq_word_count, padding] =
            finalize_encode_state<storage_unit_t,
                                  bits_per_char,
                                  letters_per_unit>(state.body);
        emit_record(state.header_start_offset,
                    header_size,
                    seq_start,
                    seq_word_count,
                    padding);
        state.reset_for_next_record();
        return parse_step_result{parse_outcome::COMPLETE, cursor};
    }

    return parse_step_result{parse_outcome::INCOMPLETE, cursor};
}

}  // namespace bioseq::io
