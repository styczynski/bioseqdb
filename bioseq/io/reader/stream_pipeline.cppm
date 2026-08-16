/**
 * @file stream_pipeline.cppm
 * @brief Bounded record streaming and source selection.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.io.reader:stream_pipeline;

import :types;
import bioseq.common.logging;
import bioseq.sequences;
import bioseq.io.options;
import bioseq.io.errors;
import bioseq.io.buffer;
import bioseq.io.fasta;
import bioseq.io.fastq;
import bioseq.io.sources;

namespace bioseq::io::detail {

/// Stream records through bounded scratch storage on the calling thread.
///
/// For each record the sink receives `begin_record(header)` once, then
/// `sink.chunk(...)` for each packed window (non-final windows are full units
/// with `logical_length == words.size()*letters_per_unit`. The final window has
/// `end_of_record == true` and `logical_length` ≤ that), then `sink.end_record()`
/// once. The `words` span remains valid only during the `chunk` call.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          sequence_file_format p_file_format,
          io_source p_source,
          typename p_sink,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string> run_sync_stream(
    p_source source, p_sink&& sink, p_failure&& failure_fn, const reader_options& options) {

    using stream_config_type = sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using stream_traits_type = typename compact_sequence_range<stream_config_type>::traits;
    constexpr std::size_t bits_per_char = stream_traits_type::bits_per_char;
    constexpr std::size_t letters_per_unit = stream_traits_type::letters_per_unit;

    buffer_slab slab(options.io.initial_buffer_size);
    std::size_t records_emitted = 0;
    bool aborted = false;
    std::string captured_error;
    std::size_t global_offset_base = 0;

    // Retain and report only the first terminal pipeline error.
    auto failure_capture = [&aborted, &captured_error, &failure_fn](std::string&& msg) {
        if (!aborted) {
            captured_error = msg;
            failure_fn(std::move(msg));
        }
        aborted = true;
    };

    // Per-record bounded scratch — never a whole record. The FSM appends the
    // header to `header_scratch` and the packed words to `seq_scratch`. The
    // streaming logic drains and clears `seq_scratch` between FSM calls so its
    // size stays within ~one slab of body. `record_start_offset` in the
    // encoder stays 0 because every record starts (and every mid-record drain
    // leaves) `seq_scratch` empty.
    std::vector<char> header_scratch;
    std::vector<p_storage_unit> seq_scratch;
    std::vector<std::uint8_t> quality_scratch; // FASTQ content is ignored here.

    bool record_open = false; // begin_record already called
    std::size_t completed_header_start = 0;
    std::size_t completed_header_size = 0;
    std::size_t completed_padding = 0; // padding of the record's last
                                       // unit, captured in the emit
                                       // callback (fires BEFORE the FSM
                                       // resets its per-record counters)

    // Clear bounded scratch after one record has been delivered.
    auto reset_record =
        [&completed_padding, &header_scratch, &quality_scratch, &record_open, &seq_scratch]() {
            header_scratch.clear();
            seq_scratch.clear();
            quality_scratch.clear();
            record_open = false;
            completed_padding = 0;
        };

    // Deliver one packed chunk and release its reusable scratch storage.
    auto emit_stream_chunk = [&options, &quality_scratch, &seq_scratch, &sink](
                                 std::uint64_t chunk_bp, bool end_of_record) {
        if constexpr (requires(p_sink& sink_ref,
                               std::vector<p_storage_unit>&& owned_words,
                               std::uint64_t bp,
                               bool eof) {
                          sink_ref.chunk_owned(std::move(owned_words), bp, eof);
                      }) {
            sink.chunk_owned(std::move(seq_scratch), chunk_bp, end_of_record);
            seq_scratch = {};
            seq_scratch.reserve(std::max<std::size_t>(
                options.io.initial_buffer_size / sizeof(p_storage_unit), 1024));
        } else {
            sink.chunk(packed_sequence_chunk<p_storage_unit>{
                std::span<const p_storage_unit>(seq_scratch.data(), seq_scratch.size()),
                chunk_bp,
                end_of_record});
            seq_scratch.clear();
        }
        quality_scratch.clear();
    };

    // Drain the completed words accumulated so far as a non-final chunk. Only
    // valid mid-record (body started ⇒ header complete). Keeps RSS bounded.
    auto drain_partial =
        [&emit_stream_chunk, &header_scratch, &record_open, &seq_scratch, &sink]() {
            if (seq_scratch.empty())
                return;
            if (!record_open) {
                sink.begin_record(std::string_view(header_scratch.data(), header_scratch.size()));
                record_open = true;
            }
            // Mid-record drained words are ALWAYS full storage units (the FSM only
            // flushes a partial unit at record finalize), so the chunk has no padding.
            const std::uint64_t chunk_bp =
                static_cast<std::uint64_t>(seq_scratch.size()) * letters_per_unit;
            emit_stream_chunk(chunk_bp, /*end_of_record=*/false);
        };

    // Emit the final chunk + end the record. The final chunk's bp is the words
    // still in `seq_scratch` (those since the last drain) minus the record's
    // trailing padding. We CANNOT read the FSM's cumulative `total_letters` here:
    // `parse_one_*_record` calls `reset_for_next_record()` (which zeroes it)
    // BEFORE returning COMPLETE, so by this point it is already 0. The padding was
    // captured in the emit callback, which fires just before that reset.
    auto complete_record = [&completed_header_size,
                            &completed_header_start,
                            &completed_padding,
                            &emit_stream_chunk,
                            &header_scratch,
                            &record_open,
                            &records_emitted,
                            &reset_record,
                            &seq_scratch,
                            &sink]() {
        if (!record_open) {
            sink.begin_record(std::string_view(header_scratch.data() + completed_header_start,
                                               completed_header_size));
            record_open = true;
        }
        const std::uint64_t scratch_bp =
            static_cast<std::uint64_t>(seq_scratch.size()) * letters_per_unit;
        const std::uint64_t final_bp =
            scratch_bp >= completed_padding ? scratch_bp - completed_padding : 0;
        emit_stream_chunk(final_bp, /*end_of_record=*/true);
        sink.end_record();
        ++records_emitted;
        reset_record();
    };

    // Preserve completion offsets before the parser resets its record state.
    auto capture_completion =
        [&completed_header_size, &completed_header_start, &completed_padding](
            std::size_t header_start, std::size_t header_size, std::size_t padding) {
            completed_header_start = header_start;
            completed_header_size = header_size;
            completed_padding = padding;
        };

    if constexpr (p_file_format == sequence_file_format::fasta) {
        fasta_parser_state<p_storage_unit, bits_per_char, letters_per_unit> parser_state;
        // Capture the completed FASTA header and final-word padding.
        auto emit_fasta = [&capture_completion](std::size_t header_start,
                                                std::size_t header_size,
                                                std::size_t,
                                                std::size_t,
                                                std::size_t padding) {
            capture_completion(header_start, header_size, padding);
        };
        while (!aborted) {
            if (slab.fill_size == slab.capacity) {
                if (!slab.make_room()) {
                    if (!slab.grow(options)) {
                        emit_failure(
                            failure_capture, "buffer_limit reached at {} bytes", slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto refill_result = source.read_into(slab.raw() + slab.fill_size, free_room);
                if (refill_result.has_error) {
                    emit_failure(failure_capture, "{}", source.last_error_message());
                    break;
                }
                slab.fill_size += refill_result.bytes_read;
                slab.is_eof = refill_result.is_eof;
            }
            bool need_refill = false;
            while (!aborted) {
                parser_state.attach_to_batch(header_scratch, seq_scratch);
                const auto step = parse_one_fasta_record<p_alphabet, p_storage_unit>(
                    parser_state,
                    slab.raw(),
                    slab.fill_size,
                    slab.is_eof,
                    slab.parse_cursor,
                    global_offset_base + slab.parse_cursor,
                    emit_fasta,
                    failure_capture);
                slab.parse_cursor = step.cursor;
                if (step.outcome == parse_outcome::complete) {
                    complete_record();
                    continue;
                }
                if (step.outcome == parse_outcome::at_end_of_file) {
                    if (record_open)
                        complete_record();
                    if (aborted)
                        return std::unexpected(captured_error);
                    return records_emitted;
                }
                if (step.outcome == parse_outcome::abort) {
                    return std::unexpected(captured_error);
                }
                drain_partial(); // INCOMPLETE: bound RSS before refill
                need_refill = true;
                break;
            }
            if (!need_refill)
                continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                if (record_open)
                    complete_record();
                if (aborted)
                    return std::unexpected(captured_error);
                return records_emitted;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }
        return std::unexpected(captured_error);

    } else {
        fastq_parser_state<p_storage_unit, bits_per_char, letters_per_unit> parser_state;
        // Capture the completed FASTQ header and final-word padding.
        auto emit_fastq = [&capture_completion](std::size_t header_start,
                                                std::size_t header_size,
                                                std::size_t,
                                                std::size_t,
                                                std::size_t padding,
                                                std::size_t,
                                                std::size_t) {
            capture_completion(header_start, header_size, padding);
        };
        while (!aborted) {
            if (slab.fill_size == slab.capacity) {
                if (!slab.make_room()) {
                    if (!slab.grow(options)) {
                        emit_failure(
                            failure_capture, "buffer_limit reached at {} bytes", slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto refill_result = source.read_into(slab.raw() + slab.fill_size, free_room);
                if (refill_result.has_error) {
                    emit_failure(failure_capture, "{}", source.last_error_message());
                    break;
                }
                slab.fill_size += refill_result.bytes_read;
                slab.is_eof = refill_result.is_eof;
            }
            bool need_refill = false;
            while (!aborted) {
                parser_state.attach_to_batch(header_scratch, seq_scratch, quality_scratch);
                const auto step = parse_one_fastq_record<p_alphabet, p_storage_unit>(
                    parser_state,
                    slab.raw(),
                    slab.fill_size,
                    slab.is_eof,
                    slab.parse_cursor,
                    global_offset_base + slab.parse_cursor,
                    options.encoder.strict_fastq,
                    emit_fastq,
                    failure_capture);
                slab.parse_cursor = step.cursor;
                if (step.outcome == parse_outcome::complete) {
                    complete_record();
                    continue;
                }
                if (step.outcome == parse_outcome::at_end_of_file) {
                    if (record_open)
                        complete_record();
                    if (aborted)
                        return std::unexpected(captured_error);
                    return records_emitted;
                }
                if (step.outcome == parse_outcome::abort) {
                    return std::unexpected(captured_error);
                }
                drain_partial();
                need_refill = true;
                break;
            }
            if (!need_refill)
                continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                if (record_open)
                    complete_record();
                if (aborted)
                    return std::unexpected(captured_error);
                return records_emitted;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }
        return std::unexpected(captured_error);
    }
}

/// the single source-selection dispatch shared by
/// `stream_sequences_file` and `read_decompressed_prefix`: with `prefer_mmap`,
/// magic-sniff for gzip, choosing gzip_source or mmap_source. With
/// `gzip_auto_detect` it uses gzip_source, otherwise posix_read_source. The chosen source is
/// passed to `on_source(source)`, which returns the same expected result type.
/// open failures are reported via `failure_fn` and returned as that same error
/// type. Factored out so the 3-way selection lives in exactly one place.
template <typename p_failure, typename p_on_source>
[[nodiscard]] auto with_selected_source(std::string_view path_view,
                                        const reader_options& options,
                                        p_failure&& failure_fn,
                                        p_on_source&& on_source)
    -> std::invoke_result_t<p_on_source&, posix_read_source&&> {
    using result_type = std::invoke_result_t<p_on_source&, posix_read_source&&>;
    if (options.io.prefer_mmap) {
        auto mmap_or_err = mmap_source::try_open_with_magic(path_view, options);
        if (mmap_or_err) {
            if (mmap_or_err->is_gzipped) {
                auto gz_or_err = gzip_source::try_open(path_view);
                if (!gz_or_err) {
                    failure_fn(std::string{gz_or_err.error()});
                    return result_type{std::unexpect, std::string{gz_or_err.error()}};
                }
                return on_source(std::move(*gz_or_err));
            }
            return on_source(std::move(mmap_or_err->source));
        }
        bioseq_log<logging_level::debug>("[bioseq_io] select: mmap unavailable for '{}': {}; "
                                         "falling back to posix_read_source",
                                         path_view,
                                         mmap_or_err.error());
    }
    if (options.io.gzip_auto_detect) {
        auto gz_or_err = gzip_source::try_open(path_view);
        if (!gz_or_err) {
            failure_fn(std::string{gz_or_err.error()});
            return result_type{std::unexpect, std::string{gz_or_err.error()}};
        }
        return on_source(std::move(*gz_or_err));
    }
    auto posix_or_err = posix_read_source::try_open(path_view);
    if (!posix_or_err) {
        failure_fn(std::string{posix_or_err.error()});
        return result_type{std::unexpect, std::string{posix_or_err.error()}};
    }
    return on_source(std::move(*posix_or_err));
}

} // namespace bioseq::io::detail
