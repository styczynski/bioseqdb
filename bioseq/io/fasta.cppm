/**
 * @file fasta.cppm
 * @brief Reads FASTA records across input-buffer refills.
 *
 * Headers and packed sequence words go directly into the batch buffers.
 * Saved parser state allows a record to continue when more input arrives.
 */
module;

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <libassert/assert.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.io.fasta;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.simd;
import bioseq.io.sinks;

export namespace bioseq::io {

/// Position within a FASTA record, kept across input refills.
enum class fasta_parser_progress : std::uint8_t {
    at_header_sigil = 0,
    in_header,
    at_body_line_start,
    finished_record,
};

/// Result of one FASTA or FASTQ parser call.
enum class parse_outcome : std::uint8_t {
    complete = 0,   ///< A record was emitted. The cursor advanced past it.
    incomplete,     ///< Need more bytes. Refill and call again.
    at_end_of_file, ///< The input is exhausted and no more bytes will arrive
    abort,          ///< A parse  error was reported through the failure callback.
};

/// Parser outcome and the input position from which the caller should continue.
struct record_parse_result {
    parse_outcome outcome = parse_outcome::incomplete;
    std::size_t cursor = 0;
};


/** Raw header ownership for streaming readers; one pending byte handles split CRLF safely. */
class raw_header_buffer {
    admitted_byte_buffer bytes_;
    std::optional<char> pending_;
    std::string_view borrowed_;
public:
    /** Append one raw fragment and freeze the header when its newline is consumed. */
    [[nodiscard]] std::expected<void, std::string> append_fragment(std::string_view fragment, bool complete) {
        if (!fragment.empty()) {
            if (pending_) {
                if (auto stored = bytes_.append(std::string_view{&*pending_, 1u}); !stored) return stored;
            }
            if (auto stored = bytes_.append(fragment.substr(0u, fragment.size() - 1u)); !stored) return stored;
            pending_ = fragment.back();
        }
        if (complete) {
            if (pending_ && *pending_ != '\r')
                if (auto stored = bytes_.append(std::string_view{&*pending_, 1u}); !stored) return stored;
            pending_.reset();
            if (auto sealed = bytes_.seal(); !sealed) return sealed;
            borrowed_ = bytes_.text();
        }
        return {};
    }
    /** Count accumulated header bytes before or after newline normalization. */
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size() + static_cast<std::size_t>(pending_.has_value()); }
    /** Borrow the completed header while its reader or receiving sink retains ownership. */
    [[nodiscard]] std::string_view view() const noexcept { return borrowed_; }
    /** Transfer the sealed owner; sinks accepting ownership must retain it through end_record. */
    [[nodiscard]] admitted_byte_buffer take() noexcept { return std::move(bytes_); }
    /** Start a new record after all parser access to the previous header ends. */
    void reset() noexcept { bytes_ = admitted_byte_buffer{}; pending_.reset(); borrowed_ = {}; }
};

/// State for one FASTA record, aligned to keep cncurrent parser states apart.
template <typename p_storage_unit, std::size_t p_bits_per_char, std::size_t p_letters_per_unit>
struct alignas(64) fasta_parser_state {
    fasta_parser_progress progress = fasta_parser_progress::at_header_sigil;
    /// Whether the next body byte begins a physical line (preserved across slabs).
    bool body_line_start = true;
    /// Start of this record's header in the batch header buffer.
    std::size_t header_start_offset = 0;
    bioseq::simd::active_target::encode_state<p_storage_unit, p_bits_per_char, p_letters_per_unit>
        body{};
    /// Batch header buffer used while this record is being parsed.
    std::vector<char>* header_storage = nullptr;
    raw_header_buffer* streamed_header = nullptr;

    /** Count the selected batch or admitted streaming header storage. */
    [[nodiscard]] std::size_t header_size() const noexcept {
        return streamed_header ? streamed_header->size() : header_storage->size();
    }
    /** Append raw header bytes through one selected ownership path. */
    [[nodiscard]] std::expected<void, std::string> append_header(std::string_view bytes, bool complete) {
        if (streamed_header) return streamed_header->append_fragment(bytes, complete);
        header_storage->insert(header_storage->end(), bytes.begin(), bytes.end());
        if (complete && header_storage->size() > header_start_offset && header_storage->back() == '\r')
            header_storage->pop_back();
        return {};
    }

    /// Reset record-local progress while keeping attached batch storage.
    void reset_for_next_record() noexcept {
        progress = fasta_parser_progress::at_header_sigil;
        body.reset();
        body_line_start = true;
    }

    /// Attaches the header and sequence buffers that receive  this record.
    void attach_to_batch(std::vector<char>& header_buf,
                         std::vector<p_storage_unit>& seq_buf) noexcept {
        header_storage = &header_buf;
        body.output_storage = &seq_buf;
    }
};

/// Continues one FASTA record from the supplied input position.
/// An incomplete result keeps the state for the next refill. At, completion,
/// the parser calls `emit_record` and resets its record state. EOF ends a
/// trailing sequence body.
///
/// The callback receives `(header_start_offset, header_size, seq_start_offset,
/// seq_word_count, padding)`, referring to the attached batch buffers.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit = std::uint64_t,
          typename p_emit_record,
          typename p_failure>
[[nodiscard]] record_parse_result parse_one_fasta_record(
    fasta_parser_state<
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
    p_emit_record&& emit_record,
    p_failure&& failure_fn) {

    using sequence_traits_type = typename compact_sequence_range<
        sequence_custom_alphabet<p_alphabet, p_storage_unit>>::traits;
    constexpr std::size_t bits_per_char = sequence_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits_type::letters_per_unit;

    DEBUG_ASSERT(state.header_storage != nullptr,
                 "fasta_parser_state must be attached to a batch via attach_to_batch");
    DEBUG_ASSERT(state.body.output_storage != nullptr,
                 "fasta_parser_state must be attached to a batch via attach_to_batch");

    // Expect '>' for another record, or report EOF when the input is exhausted.
    if (state.progress == fasta_parser_progress::at_header_sigil) {
        if (cursor >= slab_size) {
            return record_parse_result{
                is_eof ? parse_outcome::at_end_of_file : parse_outcome::incomplete, cursor};
        }
        if (slab_data[cursor] != '>') {
            emit_failure(failure_fn,
                         "expected '>' at file position {}, got byte 0x{:02x}",
                         global_offset + cursor,
                         static_cast<unsigned>(slab_data[cursor]));
            return record_parse_result{parse_outcome::abort, cursor};
        }
        // Snapshot offsets at the start of the record for emit_record.
        state.header_start_offset = state.header_size();
        state.body.record_start_offset = state.body.output_storage->size();
        cursor += 1; // consume '>'
        state.progress = fasta_parser_progress::in_header;
    }

    // Read the header through its newline and remove a trailing carriage return.
    if (state.progress == fasta_parser_progress::in_header) {
        const std::size_t remaining = slab_size - cursor;
        const std::size_t newline_offset =
            simd::scan_first_byte('\n', slab_data + cursor, remaining);
        const auto appended = state.append_header(
            {reinterpret_cast<const char*>(slab_data + cursor), newline_offset}, newline_offset != remaining);
        if (!appended) {
            emit_failure(failure_fn, "{}", appended.error());
            return record_parse_result{parse_outcome::abort, cursor};
        }
        if (newline_offset == remaining)
            return record_parse_result{parse_outcome::incomplete, slab_size};
        cursor += newline_offset + 1; // consume '\n'
        state.progress = fasta_parser_progress::at_body_line_start;
    }

    // All alphabets share bounded streaming input. A header marker is structural
    // only at a physical line start, including when a line spans input slabs.
    while (state.progress == fasta_parser_progress::at_body_line_start) {
        if (cursor >= slab_size) {
            if (is_eof) { state.progress = fasta_parser_progress::finished_record; break; }
            return record_parse_result{parse_outcome::incomplete, cursor};
        }
        if (state.body_line_start && slab_data[cursor] == '>') {
            state.progress = fasta_parser_progress::finished_record;
            break;
        }
        const auto chunk = bioseq::simd::active_target::encode_multiline_body_chunk<p_alphabet>(
            state.body, slab_data + cursor, slab_size - cursor, state.body_line_start);
        if (!chunk.encoded) {
            const auto failure_position = global_offset + cursor + chunk.encoded.consumed_bytes;
            state.body.output_storage->resize(state.body.record_start_offset);
            state.header_storage->resize(state.header_start_offset);
            state.reset_for_next_record();
            emit_failure(failure_fn, "invalid sequence byte 0x{:02x} at file position {}",
                static_cast<unsigned>(chunk.encoded.offending_byte), failure_position);
            return record_parse_result{parse_outcome::abort, cursor + chunk.encoded.consumed_bytes};
        }
        cursor += chunk.encoded.consumed_bytes;
        state.body_line_start = chunk.at_line_start;
        if (chunk.at_record_boundary || is_eof) {
            state.progress = fasta_parser_progress::finished_record;
            break;
        }
        return record_parse_result{parse_outcome::incomplete, cursor};
    }

    // Finish the packed sequence, emit its offsets, and reset for the next record
    if (state.progress == fasta_parser_progress::finished_record) {
        const std::size_t header_size = state.header_size() - state.header_start_offset;
        const std::size_t seq_start = state.body.record_start_offset;
        const auto [seq_word_count, padding] = bioseq::simd::active_target::
            finalize_encode_state<p_storage_unit, bits_per_char, letters_per_unit>(state.body);
        emit_record(state.header_start_offset, header_size, seq_start, seq_word_count, padding);
        state.reset_for_next_record();
        return record_parse_result{parse_outcome::complete, cursor};
    }

    return record_parse_result{parse_outcome::incomplete, cursor};
}

} // namespace bioseq::io
