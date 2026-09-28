/**
 * @file reader.cppm
 * @brief Reads sequence files as batches or streamed packed chunks.
 *
 * Batch and view types are re-exported from `:types`. The pipeline partitions
 * provide template definitions needed when callers instantiate the public readers.
 */
module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.io.reader;

export import :types;
// Export these dependencies so importing code can instantiate the reader
// templates with the helper definitions available.
export import :batch_pipeline;
export import :stream_pipeline;
import bioseq.common.logging;
import bioseq.sequences;
import bioseq.io.options;
import bioseq.io.simd;
import bioseq.io.sources;
import bioseq.io.compression.parallel_gzip_source;

export namespace bioseq::io {

/// Reads a FASTA or FASTQ file and sends batches to `handler_fn`.
/// Reports the first input or parse error through `failure_fn`. Returns
/// the emitted record count  or that diagnostic. `observed_backend`, when
/// supplied, receives the source selected for this read.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          typename p_path,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
read_sequences_file(p_path&& file_path,
                    p_handler&& handler_fn,
                    p_failure&& failure_fn,
                    const reader_options& options = default_reader_options,
                    reader_backend* observed_backend = nullptr) {

    static_assert(std::convertible_to<p_path, std::string_view>,
                  "file_path must be convertible to std::string_view");
    const std::string_view path_view = std::string_view{std::forward<p_path>(file_path)};

    // Report the backend selected by the source-dispatch tree.
    const auto record_backend = [&observed_backend](reader_backend backend) noexcept {
        if (observed_backend)
            *observed_backend = backend;
    };
    record_backend(reader_backend::unknown);

    // Try a mapping first and inspect gzip magic If mapping fails, the
    // gzip-auto-detect setting chooses between zlib and ordinary file reads.
    if (options.io.prefer_mmap) {
        auto mmap_or_err = mmap_source::try_open_with_magic(path_view, options);
        if (mmap_or_err) {
            if (mmap_or_err->is_gzipped) {
                const bool size_ok =
                    mmap_or_err->file_size >= options.io.parallel_gzip_min_compressed_bytes;
                if (options.io.parallel_gzip_enabled && size_ok) {
                    auto pgz_or_err = parallel_gzip_source::try_open_from_mmap(
                        std::move(mmap_or_err->source), options);
                    if (pgz_or_err) {
                        record_backend(reader_backend::parallel_gzip);
                        return detail::run_pipeline_for_source<p_alphabet,
                                                               p_file_format,
                                                               parallel_gzip_source>(
                            std::move(*pgz_or_err),
                            std::forward<p_handler>(handler_fn),
                            std::forward<p_failure>(failure_fn),
                            options);
                    }
                    bioseq_log<logging_level::debug>("[bioseq_io] parallel_gzip_source failed for "
                                                     "'{}': {}; falling back to single-thread "
                                                     "gzip_source",
                                                     path_view,
                                                     pgz_or_err.error());
                }
                // This fallback is allowed before reading starts. Restarting after a
                // decode error would send earlier records to the handler again.
                auto gz_or_err = gzip_source::try_open(path_view);
                if (!gz_or_err) {
                    failure_fn(std::string{gz_or_err.error()});
                    return std::unexpected(gz_or_err.error());
                }
                record_backend(reader_backend::gzip);
                return detail::run_pipeline_for_source<p_alphabet, p_file_format, gzip_source>(
                    std::move(*gz_or_err),
                    std::forward<p_handler>(handler_fn),
                    std::forward<p_failure>(failure_fn),
                    options);
            }
            record_backend(reader_backend::memory_mapped);
            return detail::run_pipeline_for_source<p_alphabet, p_file_format, mmap_source>(
                std::move(mmap_or_err->source),
                std::forward<p_handler>(handler_fn),
                std::forward<p_failure>(failure_fn),
                options);
        }
        bioseq_log<logging_level::debug>("[bioseq_io] mmap_source unavailable for '{}': {}; "
                                         "falling back to posix_read_source",
                                         path_view,
                                         mmap_or_err.error());
    }
    if (options.io.gzip_auto_detect) {
        auto gzip_source_or_err = gzip_source::try_open(path_view);
        if (!gzip_source_or_err) {
            failure_fn(std::string{gzip_source_or_err.error()});
            return std::unexpected(gzip_source_or_err.error());
        }
        record_backend(reader_backend::gzip);
        return detail::run_pipeline_for_source<p_alphabet, p_file_format, gzip_source>(
            std::move(*gzip_source_or_err),
            std::forward<p_handler>(handler_fn),
            std::forward<p_failure>(failure_fn),
            options);
    }
    auto posix_source_or_err = posix_read_source::try_open(path_view);
    if (!posix_source_or_err) {
        failure_fn(std::string{posix_source_or_err.error()});
        return std::unexpected(posix_source_or_err.error());
    }
    record_backend(reader_backend::posix_read);
    return detail::run_pipeline_for_source<p_alphabet, p_file_format, posix_read_source>(
        std::move(*posix_source_or_err),
        std::forward<p_handler>(handler_fn),
        std::forward<p_failure>(failure_fn),
        options);
}

/// Reads FASTA or FASTQ on the calling thread and sends packed chunks to `sink`.
/// The, caller selects the packed word type. Completed words can be delivered
/// before a record ends, avoiding a whole-record sequence batch. Header and
/// FASTQ quality parsing still  use scratch buffers. Returns the recrd count
/// or the first input or parse error.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          sequence_file_format p_file_format,
          typename p_sink,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
stream_sequences_file(const char* path,
                      p_sink&& sink,
                      p_failure&& failure_fn,
                      reader_options options = default_reader_options) {
    // Keep parsing and sink callbacks on the calling thread.
    options.concurrency.worker_count = 0;
    options.concurrency.parser_count = 1;
    const std::string_view path_view{path};

    // Share source selection with `read_decompressed_prefix`, then stream the
    // selected concrete source through `run_sync_stream`.
    return detail::with_selected_source(
        path_view, options, failure_fn, [&failure_fn, &options, &sink](auto&& src) {
            using source_type = std::remove_reference_t<decltype(src)>;
            return detail::run_sync_stream<p_alphabet, p_storage_unit, p_file_format, source_type>(
                std::move(src), sink, failure_fn, options);
        });
}

/// Reads up to `max_bytes` from the decoded start of a file for header sampling.
/// Uses the same source selection as `stream_sequences_file`.
[[nodiscard]] inline std::expected<std::string, std::string>
read_decompressed_prefix(const char* path, std::size_t max_bytes, const reader_options& options) {
    const std::string_view path_view{path};
    std::string open_error;
    // Save the first source-open error passed to this callback.
    auto on_open_fail = [&open_error](std::string&& msg) {
        if (open_error.empty())
            open_error = std::move(msg);
    };
    return detail::with_selected_source(
        path_view,
        options,
        on_open_fail,
        [&max_bytes](auto&& source) -> std::expected<std::string, std::string> {
            std::string prefix_bytes;
            prefix_bytes.resize(max_bytes);
            std::size_t total = 0;
            while (total < max_bytes) {
                const auto read_result = source.read_into(
                    reinterpret_cast<std::uint8_t*>(prefix_bytes.data()) + total,
                    max_bytes - total);
                if (read_result.has_error)
                    return std::unexpected(
                        std::string{"bioseq.load: failed to read file prefix for header sampling"});
                total += read_result.bytes_read;
                if (read_result.is_eof || read_result.bytes_read == 0)
                    break;
            }
            prefix_bytes.resize(total);
            return prefix_bytes;
        });
}

/// Sample up to `max_headers` headers from the decompressed prefix of `path`.
///
/// Leading record sigils and carriage returns are omitted. FASTQ sampling tracks
/// sequence and quality lengths so `@` inside multiline quality data is ignored.
[[nodiscard]] inline std::expected<std::vector<std::string>, std::string>
sample_sequence_headers(const char* path,
                        sequence_file_format file_type,
                        const reader_options& options,
                        std::size_t max_headers = 16,
                        std::size_t max_bytes = 256 * 1024) {
    auto prefix_result = read_decompressed_prefix(path, max_bytes, options);
    if (!prefix_result)
        return std::unexpected(prefix_result.error());
    const std::string& prefix = *prefix_result;
    const auto* base = reinterpret_cast<const std::uint8_t*>(prefix.data());
    const std::size_t prefix_size = prefix.size();

    std::vector<std::string> headers;
    headers.reserve(max_headers);
    // Normalize sampled headers from CRLF and LF inputs.
    auto strip_trailing_carriage_return = [](std::string_view text) {
        if (!text.empty() && text.back() == '\r')
            text.remove_suffix(1);
        return text;
    };
    // Locate the next line boundary within the sampled prefix.
    auto line_end_at = [&base, &prefix_size](std::size_t cursor) {
        return cursor + simd::scan_first_byte('\n', base + cursor, prefix_size - cursor);
    };

    std::size_t cursor = 0;
    if (file_type == sequence_file_format::fasta) {
        while (headers.size() < max_headers && cursor < prefix_size) {
            if ((cursor == 0 || prefix[cursor - 1] == '\n') && prefix[cursor] == '>') {
                const std::size_t line_end = line_end_at(cursor);
                headers.emplace_back(strip_trailing_carriage_return(std::string_view{prefix}.substr(
                    cursor + 1, line_end > cursor + 1 ? line_end - (cursor + 1) : 0)));
                cursor = line_end + 1;
            } else {
                const std::size_t line_end = line_end_at(cursor);
                if (line_end >= prefix_size)
                    break;
                cursor = line_end + 1;
            }
        }
    } else { // FASTQ: records may have multiline sequence / quality sections.
        while (headers.size() < max_headers && cursor < prefix_size) {
            if (prefix[cursor] != '@')
                break; // truncated/garbled prefix
            const std::size_t header_end = line_end_at(cursor);
            headers.emplace_back(strip_trailing_carriage_return(std::string_view{prefix}.substr(
                cursor + 1, header_end > cursor + 1 ? header_end - (cursor + 1) : 0)));
            cursor = header_end + 1;

            std::size_t sequence_byte_count = 0;
            while (cursor < prefix_size && prefix[cursor] != '+') {
                const std::size_t line_end = line_end_at(cursor);
                for (std::size_t byte_index = cursor;
                     byte_index < line_end && byte_index < prefix_size;
                     ++byte_index)
                    if (prefix[byte_index] != '\r')
                        ++sequence_byte_count;
                cursor = line_end + 1;
            }
            if (cursor >= prefix_size || prefix[cursor] != '+')
                break;
            cursor = line_end_at(cursor) + 1; // skip the '+' line

            std::size_t quality_byte_count = 0;
            while (cursor < prefix_size && quality_byte_count < sequence_byte_count) {
                const std::size_t line_end = line_end_at(cursor);
                for (std::size_t byte_index = cursor;
                     byte_index < line_end && byte_index < prefix_size;
                     ++byte_index)
                    if (prefix[byte_index] != '\r')
                        ++quality_byte_count;
                cursor = line_end + 1;
            }
            if (quality_byte_count < sequence_byte_count)
                break; // prefix ended mid-record
        }
    }
    return headers;
}

/// Read files in declaration order and return their combined record count.
///
/// Handler and failure callbacks are shared. Each file is drained before the
/// next file is opened.
template <sequence_alphabet p_alphabet,
          sequence_file_format p_file_format,
          typename p_path,
          typename p_handler,
          typename p_failure>
[[nodiscard]] std::expected<std::size_t, std::string>
read_sequences_files(std::span<const p_path> file_paths,
                     p_handler&& handler_fn,
                     p_failure&& failure_fn,
                     const reader_options& options = default_reader_options) {
    std::size_t total_records = 0;
    for (const auto& file_path : file_paths) {
        auto outcome = read_sequences_file<p_alphabet, p_file_format, const p_path&>(
            file_path, handler_fn, failure_fn, options);
        if (!outcome)
            return outcome;
        total_records += *outcome;
    }
    return total_records;
}

} // namespace bioseq::io
