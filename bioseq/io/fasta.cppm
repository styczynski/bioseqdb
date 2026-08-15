/**
 * @file fasta.cppm
 * @brief Per-record FASTA FSM driver.
 *
 * Walks the slab via `bioseq.io.simd` to find
 * record boundaries. The encoder pipeline lives in
 * `bioseq.sequences.simd` (see `encode_body_chunk` and the dna4 SIMD
 * primitives); this module is the FSM glue.
 *
 * The FSM appends header bytes directly into the
 * batch's shared `_header_storage`, and the encoder pushes packed
 * sequence words directly into the batch's shared `_sequence_storage`. The
 * emit-record callback only records the per-record offset / padding
 * triple. There is NO per-record `std::string` or `std::vector`
 * allocation on this path — the batch's three backing vectors grow
 * geometrically across thousands of records.
 *
 * Highway is included directly so the FSM can call
 * `bioseq::simd::active_target::encode_body_chunk*` without a
 * translation layer. This mirrors the pattern in
 * `pg_compact_sequence.cppm` and `kmers/index.cppm` (both include
 * highway and import `bioseq_sequences.simd`).
 */
module;

#include <libassert/assert.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.io.fasta;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.simd;

export namespace bioseq::io {

/// FASTA per-record FSM progress marker. Used to resume parsing after a
/// refill.
enum class fasta_parser_progress : std::uint8_t {
    at_header_sigil = 0,
    in_header,
    at_body_line_start,
    finished_record,
};

/// Outcome of a single `parse_one_fasta_record` call.
enum class parse_outcome : std::uint8_t {
    complete = 0,       ///< A record was emitted; cursor advanced past it.
    incomplete,         ///< Need more bytes; refill and call again.
    at_end_of_file,     ///< Slab is empty and EOF — no more records.
    abort,              ///< The failure callback requested termination.
};

/// Returned by `parse_one_fasta_record` / `parse_one_fastq_record` to
/// avoid an output pointer for the cursor. The caller does
/// `auto step = parse_one_*_record(...); slab.parse_cursor =
/// step.cursor;` after each call.
struct record_parse_result {
    parse_outcome outcome = parse_outcome::incomplete;
    std::size_t   cursor  = 0;
};

/// Per-record FASTA parser state. Cache-line alignment separates parser
/// states used by concurrent mmap shards.
template <typename p_storage_unit,
          std::size_t p_bits_per_char,
          std::size_t p_letters_per_unit>
struct alignas(64) fasta_parser_state {
    fasta_parser_progress progress = fasta_parser_progress::at_header_sigil;
    /// Offset into the batch's `_header_storage` where the current
    /// record's header bytes start. Captured when header parsing begins
    /// and used by the emit-record callback.
    std::size_t header_start_offset = 0;
    bioseq::simd::active_target::encode_state<
        p_storage_unit,
        p_bits_per_char,
        p_letters_per_unit> body{};
    /// Borrowed pointer to the batch's shared header buffer. The FSM
    /// pushes header bytes into this directly to avoid per-record allocation.
    std::vector<char>* header_storage = nullptr;

    /// Reset record-local progress while retaining attached batch storage.
    void reset_for_next_record() noexcept {
        progress = fasta_parser_progress::at_header_sigil;
        body.reset();
    }

    /// Called by the FSM driver to point this state at the active
    /// batch's storage. Re-bound at the top of every parse iteration
    /// so a `make_room` slide on the producer side that triggers a
    /// realloc on the batch's vector is observed before the next
    /// emit-record.
    void attach_to_batch(std::vector<char>& header_buf,
                         std::vector<p_storage_unit>& seq_buf) noexcept {
        header_storage = &header_buf;
        body.output_storage = &seq_buf;
    }
};

/// Parse one FASTA record from the slab, given the state. On COMPLETE,
/// the caller appends the record to its batch and recycles the state.
/// `slab_data` / `slab_size` are the readable region. `cursor` is the
/// current parse offset and the result carries its updated value.
///
/// `is_eof` is the producer's "no more bytes coming" flag for this
/// slab; lets us emit the trailing record cleanly.
///
/// `emit_record` is invoked with
/// `(header_start_offset, header_size, seq_start_offset, seq_word_count, padding)`
/// — offsets into the batch's shared storage, plus the encoder's
/// `(word_count, padding)` summary.
template <
    sequence_alphabet p_alphabet,
    typename p_storage_unit = std::uint64_t,
    typename p_emit_record,
    typename p_failure>
[[nodiscard]] record_parse_result parse_one_fasta_record(
    fasta_parser_state<p_storage_unit,
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
    p_emit_record&& emit_record,
    p_failure&& failure_fn) {

    using sequence_traits_type =
        typename compact_sequence_range<
            sequence_custom_alphabet<p_alphabet, p_storage_unit>>::traits;
    constexpr std::size_t bits_per_char =
        sequence_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit =
        sequence_traits_type::letters_per_unit;

    DEBUG_ASSERT(state.header_storage != nullptr,
                 "fasta_parser_state must be attached to a batch via attach_to_batch");
    DEBUG_ASSERT(state.body.output_storage != nullptr,
                 "fasta_parser_state must be attached to a batch via attach_to_batch");

    // Step 1: AT_HEADER_SIGIL — expect '>' or EOF.
    if (state.progress == fasta_parser_progress::at_header_sigil) {
        if (cursor >= slab_size) {
            return record_parse_result{
                is_eof ? parse_outcome::at_end_of_file : parse_outcome::incomplete,
                cursor};
        }
        if (slab_data[cursor] != '>') {
            emit_failure(failure_fn,
                         "expected '>' at file position {}, got byte 0x{:02x}",
                         global_offset + cursor,
                         static_cast<unsigned>(slab_data[cursor]));
            return record_parse_result{parse_outcome::abort, cursor};
        }
        // Snapshot offsets at the start of the record for emit_record.
        state.header_start_offset = state.header_storage->size();
        state.body.record_start_offset = state.body.output_storage->size();
        cursor += 1;  // consume '>'
        state.progress = fasta_parser_progress::in_header;
    }

    // Step 2: IN_HEADER — scan to '\n', strip optional '\r'.
    if (state.progress == fasta_parser_progress::in_header) {
        const std::size_t remaining = slab_size - cursor;
        const std::size_t newline_offset = simd::scan_first_byte(
            '\n', slab_data + cursor, remaining);
        if (newline_offset == remaining) {
            state.header_storage->insert(
                state.header_storage->end(),
                reinterpret_cast<const char*>(slab_data + cursor),
                reinterpret_cast<const char*>(slab_data + cursor + remaining));
            return record_parse_result{parse_outcome::incomplete, slab_size};
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
        state.progress = fasta_parser_progress::at_body_line_start;
    }

    // Step 3 & 4: body lines, looking for the next '>' that terminates this record.
    //
    // Two strategies depending on alphabet:
    //   * dna4 + uint64 (the canonical hot path): scan once for the
    //     next '>' and feed the whole multi-line body span to the
    //     SIMD encoder. The encoder's multiline path filters
    //     newlines via its low-nibble LUT (which already maps `\n`
    //     and `\r` to skip-sentinels) and lane-compacts the valid
    //     bytes branchlessly. Eliminates per-line FSM overhead — for
    //     a 234 MiB human-genome chromosome with ~125 K body lines
    //     this is ~3 M state-machine iterations collapsed to one
    //     pass.
    //   * Other alphabets / storage units: fall back to the per-line
    //     loop with the scalar encoder. The multiline encoder is a
    //     dna4-uint64 specialisation.
    constexpr bool use_multiline_encoder =
        std::is_same_v<p_alphabet, seq::dna4>
        && std::is_same_v<p_storage_unit, std::uint64_t>
        && bits_per_char == 2 && letters_per_unit == 32;

    if constexpr (use_multiline_encoder) {
        while (state.progress == fasta_parser_progress::at_body_line_start) {
            if (cursor >= slab_size) {
                if (is_eof) {
                    state.progress = fasta_parser_progress::finished_record;
                    break;
                }
                return record_parse_result{parse_outcome::incomplete, cursor};
            }
            if (slab_data[cursor] == '>') {
                state.progress = fasta_parser_progress::finished_record;
                break;
            }
            const std::size_t remaining = slab_size - cursor;
            // Single-pass fused encode + record-boundary scan. The
            // encoder walks the body span, packing dna4 bytes
            // (filtering `\n` / `\r` via the low-nibble LUT) and
            // detecting `>` per chunk. Returns the number of bytes
            // consumed; `consumed < remaining` means a `>` was found
            // — the FSM transitions to FINISHED_RECORD. Eliminates
            // the previous separate `simd::scan_first_byte('>', …)`
            // pass over the body, halving the memory traffic.
            const std::size_t consumed =
                bioseq::simd::active_target::encode_body_chunk_dna4_simd_multiline_until_byte<
                    std::uint8_t{'>'}, std::uint64_t, 2, 32>(
                        state.body, slab_data + cursor, remaining,
                        /*newline_count_out=*/nullptr);
            cursor += consumed;
            if (consumed < remaining) {
                state.progress = fasta_parser_progress::finished_record;
                break;
            }
            // No '>' found — either EOF closes the record or we need
            // a refill.
            if (is_eof) {
                state.progress = fasta_parser_progress::finished_record;
                break;
            }
            return record_parse_result{parse_outcome::incomplete, cursor};
        }
    } else {
        while (state.progress == fasta_parser_progress::at_body_line_start) {
            if (cursor >= slab_size) {
                if (is_eof) {
                    state.progress = fasta_parser_progress::finished_record;
                    break;
                }
                return record_parse_result{parse_outcome::incomplete, cursor};
            }
            if (slab_data[cursor] == '>') {
                state.progress = fasta_parser_progress::finished_record;
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
                    bioseq::simd::active_target::encode_body_chunk<
                        p_alphabet,
                        bioseq::simd::active_target::unknown_byte_policy::drop,
                        p_storage_unit, bits_per_char, letters_per_unit>(
                            state.body, slab_data + cursor, line_size);
                    cursor += remaining;
                    state.progress = fasta_parser_progress::finished_record;
                    break;
                }
                return record_parse_result{parse_outcome::incomplete, cursor};
            }
            std::size_t line_size = newline_offset;
            if (line_size > 0 && slab_data[cursor + line_size - 1] == '\r') {
                line_size -= 1;
            }
            bioseq::simd::active_target::encode_body_chunk<
                p_alphabet,
                bioseq::simd::active_target::unknown_byte_policy::drop,
                p_storage_unit, bits_per_char, letters_per_unit>(
                    state.body, slab_data + cursor, line_size);
            cursor += newline_offset + 1;  // consume '\n'
        }
    }

    // Step 5: FINISHED_RECORD — finalise, emit, reset.
    if (state.progress == fasta_parser_progress::finished_record) {
        const std::size_t header_size =
            state.header_storage->size() - state.header_start_offset;
        const std::size_t seq_start = state.body.record_start_offset;
        const auto [seq_word_count, padding] =
            bioseq::simd::active_target::finalize_encode_state<p_storage_unit,
                                                  bits_per_char,
                                                  letters_per_unit>(state.body);
        emit_record(state.header_start_offset,
                    header_size,
                    seq_start,
                    seq_word_count,
                    padding);
        state.reset_for_next_record();
        return record_parse_result{parse_outcome::complete, cursor};
    }

    return record_parse_result{parse_outcome::incomplete, cursor};
}

}  // namespace bioseq::io
