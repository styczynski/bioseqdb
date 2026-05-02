module;

#include "common.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq_io.public_api;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.options;
import bioseq_io.errors;
import bioseq_io.buffer;
import bioseq_io.encoder;
import bioseq_io.fasta;
import bioseq_io.fastq;
import bioseq_io.sources;

export namespace bioseq::io {

/// A batch of records delivered to the user's handler. For FASTA
/// instantiations the `quality(i)` span is empty; for FASTQ it carries
/// the raw Phred+33 bytes. v1 holds owned strings/spans; the spec's
/// `[[no_unique_address]]` arena optimisation is phase-2.
template <typename C>
class sequence_batch {
public:
    using sequence_range_t = compact_sequence_range<C>;

    [[nodiscard]] std::size_t size() const noexcept { return _headers.size(); }
    [[nodiscard]] bool empty() const noexcept { return _headers.empty(); }

    [[nodiscard]] std::string_view header(std::size_t index) const noexcept {
        return _headers[index];
    }
    [[nodiscard]] const sequence_range_t& sequence(std::size_t index) const noexcept {
        return _sequences[index];
    }
    [[nodiscard]] std::span<const std::uint8_t> quality(std::size_t index) const noexcept {
        if (_qualities.size() <= index) return {};
        return std::span<const std::uint8_t>(_qualities[index]);
    }

    void append_fasta(std::string&& header, sequence_range_t&& sequence_range) {
        _headers.push_back(std::move(header));
        _sequences.push_back(std::move(sequence_range));
    }

    void append_fastq(std::string&& header,
                      sequence_range_t&& sequence_range,
                      std::vector<std::uint8_t>&& quality_bytes) {
        _headers.push_back(std::move(header));
        _sequences.push_back(std::move(sequence_range));
        _qualities.push_back(std::move(quality_bytes));
    }

    void clear() noexcept {
        _headers.clear();
        _sequences.clear();
        _qualities.clear();
    }

private:
    std::vector<std::string> _headers;
    std::vector<sequence_range_t> _sequences;
    std::vector<std::vector<std::uint8_t>> _qualities;
};

/// Concept matching the spec §3.5: a handler that accepts a batch
/// rvalue.
template <typename handler_fn_t, typename batch_t>
concept sequence_batch_handler = std::invocable<handler_fn_t, batch_t&&>;

namespace detail {

/// Run the synchronous read loop with a concrete source. Pulled out so
/// `read_sequences_file` can dispatch on a runtime backend choice
/// without duplicating the body. v1 only: synchronous mode (one
/// thread). Spec §12.6 + Appendix M.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    io_source source_t,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string> run_sync_pipeline(
    source_t source,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options) {

    using alphabet_config = sequence_custom_alphabet<alphabet_t, std::uint64_t>;
    using sequence_range_t = compact_sequence_range<alphabet_config>;
    using batch_t = sequence_batch<alphabet_config>;
    using sequence_traits = typename sequence_range_t::traits;

    constexpr std::size_t bits_per_char = sequence_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = sequence_traits::letters_per_unit;

    buffer_slab slab(options.io.initial_buffer_size);
    batch_t batch;
    std::size_t records_emitted = 0;
    bool aborted = false;
    std::string captured_error;
    std::size_t global_offset_base = 0;  // bytes consumed from earlier slab fills

    auto failure_capture = [&](std::string&& msg) {
        if (!aborted) {
            captured_error = msg;
            failure_fn(std::move(msg));
        }
        aborted = true;
    };

    auto flush_batch = [&]() {
        if (batch.empty()) return;
        handler_fn(std::move(batch));
        batch = batch_t{};
    };

    const std::size_t flush_record_threshold =
        (options.concurrency.batch_target_record_count > 0)
            ? options.concurrency.batch_target_record_count
            : 4096;

    if constexpr (file_type == sequence_io_file_type::FASTA) {
        fasta_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;

        auto emit_fasta_record = [&](std::string&& header,
                                     std::vector<std::uint64_t>&& packed,
                                     std::size_t padding) {
            batch.append_fasta(
                std::move(header),
                sequence_range_t::from_packed(std::move(packed), padding));
            ++records_emitted;
            if (batch.size() >= flush_record_threshold) flush_batch();
        };

        while (!aborted) {
            // Refill: make room then read into free tail.
            if (slab.fill_size == slab.capacity) {
                if (!slab.make_room()) {
                    if (!slab.grow(options)) {
                        emit_failure(failure_capture,
                                     "buffer_limit reached at {} bytes",
                                     slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto rr = source.read_into(slab.raw() + slab.fill_size, free_room);
                if (rr.has_error) {
                    emit_failure(failure_capture, "{}", source.last_error_message());
                    break;
                }
                slab.fill_size += rr.bytes_read;
                slab.is_eof = rr.is_eof;
            }

            // Parse as much as possible.
            bool need_refill = false;
            while (!aborted) {
                std::size_t cursor = slab.parse_cursor;
                const auto outcome = parse_one_fasta_record<alphabet_t, std::uint64_t>(
                    parser_state,
                    slab.raw(),
                    slab.fill_size,
                    slab.is_eof,
                    &cursor,
                    global_offset_base + slab.parse_cursor,
                    emit_fasta_record,
                    failure_capture);
                slab.parse_cursor = cursor;
                if (outcome == parse_outcome::COMPLETE) continue;
                if (outcome == parse_outcome::AT_EOF) {
                    flush_batch();
                    if (aborted) {
                        return std::unexpected(captured_error);
                    }
                    return records_emitted;
                }
                if (outcome == parse_outcome::ABORT) {
                    return std::unexpected(captured_error);
                }
                need_refill = true;
                break;
            }
            if (!need_refill) continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                flush_batch();
                if (aborted) {
                    return std::unexpected(captured_error);
                }
                return records_emitted;
            }
            // Refill loop continues.
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }
        return std::unexpected(captured_error);

    } else {
        fastq_parser_state<std::uint64_t, bits_per_char, letters_per_unit> parser_state;

        auto emit_fastq_record = [&](std::string&& header,
                                     std::vector<std::uint64_t>&& packed,
                                     std::size_t padding,
                                     std::vector<std::uint8_t>&& quality_bytes) {
            batch.append_fastq(
                std::move(header),
                sequence_range_t::from_packed(std::move(packed), padding),
                std::move(quality_bytes));
            ++records_emitted;
            if (batch.size() >= flush_record_threshold) flush_batch();
        };

        while (!aborted) {
            if (slab.fill_size == slab.capacity) {
                if (!slab.make_room()) {
                    if (!slab.grow(options)) {
                        emit_failure(failure_capture,
                                     "buffer_limit reached at {} bytes",
                                     slab.capacity);
                        break;
                    }
                }
            }
            const std::size_t free_room = slab.capacity - slab.fill_size;
            if (free_room > 0 && !slab.is_eof) {
                const auto rr = source.read_into(slab.raw() + slab.fill_size, free_room);
                if (rr.has_error) {
                    emit_failure(failure_capture, "{}", source.last_error_message());
                    break;
                }
                slab.fill_size += rr.bytes_read;
                slab.is_eof = rr.is_eof;
            }
            bool need_refill = false;
            while (!aborted) {
                std::size_t cursor = slab.parse_cursor;
                const auto outcome = parse_one_fastq_record<alphabet_t, std::uint64_t>(
                    parser_state,
                    slab.raw(),
                    slab.fill_size,
                    slab.is_eof,
                    &cursor,
                    global_offset_base + slab.parse_cursor,
                    emit_fastq_record,
                    failure_capture);
                slab.parse_cursor = cursor;
                if (outcome == parse_outcome::COMPLETE) continue;
                if (outcome == parse_outcome::AT_EOF) {
                    flush_batch();
                    if (aborted) {
                        return std::unexpected(captured_error);
                    }
                    return records_emitted;
                }
                if (outcome == parse_outcome::ABORT) {
                    return std::unexpected(captured_error);
                }
                need_refill = true;
                break;
            }
            if (!need_refill) continue;
            if (slab.is_eof && slab.parse_cursor == slab.fill_size) {
                flush_batch();
                if (aborted) {
                    return std::unexpected(captured_error);
                }
                return records_emitted;
            }
            global_offset_base += slab.parse_cursor;
            (void)slab.make_room();
        }
        return std::unexpected(captured_error);
    }
}

}  // namespace detail

/// Public entry point. Reads FASTA / FASTQ from a file path, emits
/// per-batch records via `handler_fn`, and reports the first terminal
/// failure via `failure_fn`. Returns the count of records emitted on
/// success, or the diagnostic message on failure (as a `std::expected`).
/// Spec §3.5.
template <
    sequence_alphabet alphabet_t,
    sequence_io_file_type file_type,
    typename PathLike,
    typename handler_fn_t,
    typename failure_fn_t>
[[nodiscard]] std::expected<std::size_t, std::string> read_sequences_file(
    PathLike&& file_path,
    handler_fn_t&& handler_fn,
    failure_fn_t&& failure_fn,
    const reader_options& options = default_reader_options) {

    static_assert(std::convertible_to<PathLike, std::string_view>,
                  "file_path must be convertible to std::string_view");
    const std::string_view path_view = std::string_view{
        std::forward<PathLike>(file_path)};

    if (options.io.gzip_auto_detect) {
        auto gzip_or_err = gzip_source::try_open(path_view);
        if (!gzip_or_err) {
            failure_fn(std::string{gzip_or_err.error()});
            return std::unexpected(gzip_or_err.error());
        }
        return detail::run_sync_pipeline<alphabet_t, file_type, gzip_source>(
            std::move(*gzip_or_err),
            std::forward<handler_fn_t>(handler_fn),
            std::forward<failure_fn_t>(failure_fn),
            options);
    }
    auto posix_or_err = posix_read_source::try_open(path_view);
    if (!posix_or_err) {
        failure_fn(std::string{posix_or_err.error()});
        return std::unexpected(posix_or_err.error());
    }
    return detail::run_sync_pipeline<alphabet_t, file_type, posix_read_source>(
        std::move(*posix_or_err),
        std::forward<handler_fn_t>(handler_fn),
        std::forward<failure_fn_t>(failure_fn),
        options);
}

}  // namespace bioseq::io
